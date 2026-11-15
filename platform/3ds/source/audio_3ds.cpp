#include "audio_3ds.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

namespace starfox::platform_3ds {
namespace {
// Microseconds for the packet timing diagnostics: the ARM11 system tick on
// 3DS, the stub's millisecond clock in host tests.
std::uint64_t audio_now_us() {
#ifdef __3DS__
    const auto ticks=svcGetSystemTick();
    return (ticks/SYSCLOCK_ARM11)*1000000ULL+(ticks%SYSCLOCK_ARM11)*1000000ULL/SYSCLOCK_ARM11;
#else
    return osGetTime()*1000ULL;
#endif
}
}


// push_samples() derives the real CSND rate from this timer value:
// 0x3FEC3FC / 2094, about 32,009.5 frames per second.
static_assert(CSND_TIMER(audio::Spc700Audio::sample_rate) == 2094U);

Audio3ds::Audio3ds() {
    // Never create the SPC's independent stem worker inside this wrapper.
    // It could otherwise contend with our complete-tick worker for core 1.
    spc_.set_parallel_stems_enabled(false);
}

Audio3ds::~Audio3ds() { close(); }

bool Audio3ds::open() {
    close();
    diagnostics_ = {};
    spc_.set_parallel_stems_enabled(false);
    // NDSP initialization data-aborts on the tested retail console before it
    // can return a Result. CSND is a separate sound service and fails cleanly
    // when it cannot be acquired. SPC emulation still runs if audio is muted.
    diagnostics_.csnd_result = csndInit();
    if (R_SUCCEEDED(diagnostics_.csnd_result)) {
        ready_ = (csndChannels & (1U << left_channel_)) != 0
            && (csndChannels & (1U << right_channel_)) != 0;
        if (ready_) {
            pcm_left_ = static_cast<std::int16_t*>(linearAlloc(ring_bytes_));
            pcm_right_ = static_cast<std::int16_t*>(linearAlloc(ring_bytes_));
            ready_ = pcm_left_ != nullptr && pcm_right_ != nullptr;
        }
        if (!ready_) csndExit();
    }

    LightEvent_Init(&work_requested_, RESET_ONESHOT);
    LightEvent_Init(&work_completed_, RESET_ONESHOT);
    LightEvent_Init(&ports_completed_, RESET_ONESHOT);
    stop_worker_.store(false, std::memory_order_release);

    diagnostics_.model_result = APT_CheckNew3DS(&diagnostics_.new_3ds);
    if (R_SUCCEEDED(diagnostics_.model_result) && diagnostics_.new_3ds) {
        // Core 2 has its own New 3DS application entitlement in the exheader.
        // It does not need an APT slice from the system/GSP core.
        diagnostics_.core2_attempted = true;
        worker_ = threadCreate(&Audio3ds::worker_main, this, 128U * 1024U,
                               0x30, 2, false);
        diagnostics_.core2_created = worker_ != nullptr;
        if (worker_ != nullptr) diagnostics_.selected_core = 2;
    }

    if (R_FAILED(diagnostics_.model_result) || !diagnostics_.new_3ds) {
        // Share the cartridge DSP voices. Old 3DS has no core 2: compute the
        // SPC beside the game on the system core when APT grants a large
        // enough slice (the approach used by other Old 3DS ports), and fall
        // back to the application core otherwise.
        spc_ = audio::Spc700Audio{audio::Spc700Audio::Routing::cartridge};
        spc_.set_parallel_stems_enabled(false);
        start_core1_worker({80U, 70U, 50U});
        if (worker_ == nullptr) diagnostics_.selected_core = 0;
    } else if (worker_ == nullptr) {
        // If New 3DS core 2 was unavailable (or model detection failed),
        // retain the verified core-1 fallback and restore the prior quota.
        start_core1_worker({30U, 20U, 10U});
        if (worker_ == nullptr) diagnostics_.selected_core = 0;
    }
    return ready_;
}

void Audio3ds::start_core1_worker(std::initializer_list<u32> quotas) {
    diagnostics_.quota_query_result =
        APT_GetAppCpuTimeLimit(&diagnostics_.original_quota);
    if (R_FAILED(diagnostics_.quota_query_result)) return;
    diagnostics_.effective_quota = diagnostics_.original_quota;
    for (const u32 requested : quotas) {
        ++diagnostics_.core1_attempts;
        diagnostics_.quota_set_result = APT_SetAppCpuTimeLimit(requested);
        if (R_FAILED(diagnostics_.quota_set_result)) continue;
        quota_changed_ = true;
        u32 granted = 0;
        diagnostics_.quota_verify_result = APT_GetAppCpuTimeLimit(&granted);
        if (R_SUCCEEDED(diagnostics_.quota_verify_result) && granted == requested) {
            diagnostics_.effective_quota = granted;
            worker_ = threadCreate(&Audio3ds::worker_main, this,
                128U * 1024U, 0x30, 1, false);
            if (worker_ != nullptr) {
                diagnostics_.selected_core = 1;
                return;
            }
        }
        diagnostics_.quota_restore_result =
            APT_SetAppCpuTimeLimit(diagnostics_.original_quota);
        if (R_FAILED(diagnostics_.quota_restore_result)) return;
        quota_changed_ = false;
        diagnostics_.effective_quota = diagnostics_.original_quota;
    }
}

void Audio3ds::close() noexcept {
    if (worker_ != nullptr) {
        while (completed_.load(std::memory_order_acquire) != submitted_)
            LightEvent_Wait(&work_completed_);
        stop_worker_.store(true, std::memory_order_release);
        LightEvent_Signal(&work_requested_);
        static_cast<void>(threadJoin(worker_, U64_MAX));
        threadFree(worker_);
        worker_ = nullptr;
    }
    if (quota_changed_) {
        diagnostics_.quota_restore_result =
            APT_SetAppCpuTimeLimit(diagnostics_.original_quota);
        if (R_SUCCEEDED(diagnostics_.quota_restore_result)) {
            diagnostics_.effective_quota = diagnostics_.original_quota;
            quota_changed_ = false;
        }
    }
    spc_.set_parallel_stems_enabled(false);
    submitted_ = collected_ = 0U;
    handed_.store(0U, std::memory_order_relaxed);
    ported_.store(0U, std::memory_order_relaxed);
    completed_.store(0U, std::memory_order_relaxed);
    for (auto& packet : packets_) {
        packet.writes.clear();
        packet.pcm.clear();
        packet.failure = packet.ports_failure = nullptr;
    }
    // Closing from HOME wakes the application without the foreground it had;
    // submit the stop and let csndExit release the channels.
    stop_playback(false);
    if (ready_) {
        csndExit();
    }
    ready_ = false;
    if (pcm_left_ != nullptr) linearFree(pcm_left_);
    if (pcm_right_ != nullptr) linearFree(pcm_right_);
    pcm_left_ = nullptr;
    pcm_right_ = nullptr;
}

void Audio3ds::stop_playback() noexcept {
    stop_playback(true);
}

void Audio3ds::stop_playback(bool wait_done) noexcept {
    if (playing_) {
        CSND_SetPlayState(left_channel_, 0);
        CSND_SetPlayState(right_channel_, 0);
        static_cast<void>(CSND_UpdateInfo(wait_done));
    }
    playing_ = false;
    playback_started_ms_ = 0;
    write_frame_ = 0;
    silence_until_frame_ = 0;
    source_frames_total_ = 0;
    output_frames_total_ = 0;
}

void Audio3ds::silence_for_suspend() noexcept {
    if (!playing_) return;
    std::memset(pcm_left_, 0, ring_bytes_);
    std::memset(pcm_right_, 0, ring_bytes_);
    static_cast<void>(GSPGPU_FlushDataCache(pcm_left_, ring_bytes_));
    static_cast<void>(GSPGPU_FlushDataCache(pcm_right_, ring_bytes_));
}

void Audio3ds::reapply_cpu_quota() noexcept {
    if (quota_changed_)
        static_cast<void>(APT_SetAppCpuTimeLimit(diagnostics_.effective_quota));
}

void Audio3ds::restart_spc() {
    if (pending()) static_cast<void>(finish_update());
    stop_playback();
    spc_ = audio::Spc700Audio{spc_.routing()};
    spc_.set_parallel_stems_enabled(false);
}

std::size_t Audio3ds::prime_upload_sequence(
    std::span<const simulation::ApuPortWrite> writes) {
    return spc_.prime_upload_sequence(writes);
}

bool Audio3ds::update(std::span<const simulation::ApuPortWrite> writes) {
    begin_update(writes);
    return finish_update();
}

void Audio3ds::worker_main(void* argument) {
    auto& self = *static_cast<Audio3ds*>(argument);
    for (;;) {
        LightEvent_Wait(&self.work_requested_);
        if (self.stop_worker_.load(std::memory_order_acquire)) break;
        // Every packet handed over so far, in order, without going back to
        // sleep in between.
        for (;;) {
            const auto number = self.completed_.load(std::memory_order_relaxed);
            if (number == self.handed_.load(std::memory_order_acquire)) break;
            auto& packet = self.packets_[number % queue_slots_];
            try {
                const auto begin_us = audio_now_us();
                packet.queue_us = static_cast<std::uint32_t>(begin_us - packet.request_us);
                const auto begin_ms = osGetTime();
                self.spc_.render_logic_tick_with_ports(packet.writes, packet.pcm,
                    &Audio3ds::publish_ports, &self);
                packet.compute_ms = static_cast<std::uint32_t>(osGetTime() - begin_ms);
                packet.compute_us = static_cast<std::uint32_t>(audio_now_us() - begin_us);
            } catch (...) {
                packet.failure = std::current_exception();
            }
            if (self.ported_.load(std::memory_order_relaxed) == number) {
                // It failed before its ports were out.
                packet.ports_failure = packet.failure;
                publish_ports(&self, {});
            }
            self.completed_.store(number + 1U, std::memory_order_release);
            LightEvent_Signal(&self.work_completed_);
        }
    }
}

// Called by the SPC, on the worker, once per packet.
void Audio3ds::publish_ports(void* context,
    const std::array<std::uint8_t,4>& ports) noexcept {
    auto& self = *static_cast<Audio3ds*>(context);
    const auto number = self.ported_.load(std::memory_order_relaxed);
    if (number != self.completed_.load(std::memory_order_relaxed)) return;   // already out
    auto& packet = self.packets_[number % queue_slots_];
    packet.ports = ports;
    packet.ports_ready_us = static_cast<std::uint32_t>(audio_now_us() - packet.request_us);
    self.ported_.store(number + 1U, std::memory_order_release);
    LightEvent_Signal(&self.ports_completed_);
}

std::array<std::uint8_t,4> Audio3ds::wait_for_output_ports() {
    if (!pending() || worker_ == nullptr) return spc_.output_ports();
    const auto begin = audio_now_us();
    // The ports of the last packet handed over: the ones a tick would have
    // found had it waited for every packet before starting the next.
    while (ported_.load(std::memory_order_acquire) != submitted_)
        LightEvent_Wait(&ports_completed_);
    const auto& packet = packets_[(submitted_ - 1U) % queue_slots_];
    diagnostics_.ports_ready_last_us = packet.ports_ready_us;
    diagnostics_.ports_wait_last_us = static_cast<std::uint32_t>(audio_now_us()-begin);
    diagnostics_.wait_total_us += diagnostics_.ports_wait_last_us;
    if (packet.ports_failure) std::rethrow_exception(packet.ports_failure);
    known_ports_ = packet.ports;
    return known_ports_;
}

void Audio3ds::begin_update(
    std::span<const simulation::ApuPortWrite> writes) {
    if (worker_ == nullptr) {
        // No worker: one packet at a time, rendered here.
        if (pending()) static_cast<void>(finish_update());
        auto& packet = packets_[submitted_ % queue_slots_];
        packet.request_us = audio_now_us();
        packet.failure = packet.ports_failure = nullptr;
        const auto begin_ms = osGetTime();
        spc_.render_logic_tick(writes, packet.pcm);
        packet.compute_ms = static_cast<std::uint32_t>(osGetTime() - begin_ms);
        packet.compute_us = static_cast<std::uint32_t>(audio_now_us() - packet.request_us);
        packet.queue_us = 0U;
        packet.ports = spc_.output_ports();
        ++submitted_;
        handed_.store(submitted_, std::memory_order_relaxed);
        ported_.store(submitted_, std::memory_order_relaxed);
        completed_.store(submitted_, std::memory_order_relaxed);
        return;
    }
    if (!pending()) known_ports_ = spc_.output_ports();
    // Every slot taken: the oldest packet has to come out first.
    while (submitted_ - collected_ == queue_slots_) {
        const auto wait_begin = audio_now_us();
        while (completed_.load(std::memory_order_acquire) == collected_)
            LightEvent_Wait(&work_completed_);
        diagnostics_.wait_last_us = static_cast<std::uint32_t>(audio_now_us() - wait_begin);
        diagnostics_.wait_total_us += diagnostics_.wait_last_us;
        static_cast<void>(collect_packet(collected_ + 1U));
    }
    auto& packet = packets_[submitted_ % queue_slots_];
    packet.writes.assign(writes.begin(), writes.end());
    packet.failure = packet.ports_failure = nullptr;
    packet.request_us = audio_now_us();
    ++submitted_;
    handed_.store(submitted_, std::memory_order_release);
    LightEvent_Signal(&work_requested_);
}

bool Audio3ds::collect_packet(std::uint32_t count) {
    auto& packet = packets_[(count - 1U) % queue_slots_];
    collected_ = count;
    known_ports_ = packet.ports;
    if (!packet.failure) {
        diagnostics_.spc_compute_last_ms = packet.compute_ms;
        diagnostics_.spc_compute_last_us = packet.compute_us;
        diagnostics_.worker_queue_last_us = packet.queue_us;
        diagnostics_.ports_ready_last_us = packet.ports_ready_us;
        diagnostics_.spc_compute_total_ms += packet.compute_ms;
        diagnostics_.spc_compute_max_ms = std::max(
            diagnostics_.spc_compute_max_ms, packet.compute_ms);
        ++diagnostics_.spc_compute_packets;
    }
    if (packet.failure) std::rethrow_exception(packet.failure);
    return push_samples(packet.pcm);
}

bool Audio3ds::collect() {
    bool any = false;
    while (collected_ != completed_.load(std::memory_order_acquire)) {
        static_cast<void>(collect_packet(collected_ + 1U));
        any = true;
    }
    return any;
}

bool Audio3ds::finish_update() {
    if (!pending()) return false;
    const auto wait_begin=audio_now_us();
    while (completed_.load(std::memory_order_acquire) != submitted_)
        LightEvent_Wait(&work_completed_);
    diagnostics_.wait_last_us=static_cast<std::uint32_t>(audio_now_us()-wait_begin);
    diagnostics_.wait_total_us+=diagnostics_.wait_last_us;
    bool pushed = false;
    while (collected_ != submitted_) pushed = collect_packet(collected_ + 1U);
    return pushed;
}

bool Audio3ds::push_samples(std::span<const std::int16_t> interleaved) {
    if (!ready_ || interleaved.size() != packet_samples_) return false;
    // CSND has no application-visible playback cursor. A two-second ring and
    // a 20 ms write guard keep CPU writes away from the estimated read head.
    // The hardware channel is started once; restarting a 50 ms one-shot on
    // each logic tick would truncate or gap the waveform whenever frame time
    // varies. A missed producer deadline is recorded instead of hidden.
    if (!playing_) {
        std::memset(pcm_left_, 0, ring_bytes_);
        std::memset(pcm_right_, 0, ring_bytes_);
        write_frame_ = initial_lead_frames_;
        silence_until_frame_ = 0;
        source_frames_total_ = 0;
        output_frames_total_ = 0;
    }
    // CSND's integer timer for a nominal 32 kHz request actually clocks at
    // about 32,009.5 frames/s. Follow that rate so the estimated read head
    // does not drift past the write guard during a long play session.
    const std::uint64_t played_frames = playing_
        ? (osGetTime() - playback_started_ms_) * 0x3FEC3FCULL
            / (static_cast<std::uint64_t>(
                CSND_TIMER(audio::Spc700Audio::sample_rate)) * 1000ULL)
        : 0;
    const std::uint64_t safe_frame = played_frames + write_guard_frames_;
    const std::uint64_t previous_end = write_frame_;
    // The hardware timer rounds 32,000 Hz to about 32,009.5 Hz. Carry the
    // fractional difference across packets and insert one interpolated frame
    // roughly every other tick. The SPC timeline and APU port timing stay at
    // their native 32 kHz / 20 Hz; only the CSND output is rate matched.
    source_frames_total_ += packet_frames_;
    const std::uint64_t next_output_total = source_frames_total_
        * 0x3FEC3FCULL
        / (static_cast<std::uint64_t>(
            CSND_TIMER(audio::Spc700Audio::sample_rate))
            * audio::Spc700Audio::sample_rate);
    const std::size_t output_frames = static_cast<std::size_t>(
        next_output_total - output_frames_total_);
    output_frames_total_ = next_output_total;
    diagnostics_.rate_adjust_frames += output_frames - packet_frames_;
    if (write_frame_ < safe_frame) {
        ++diagnostics_.late_packets;
        diagnostics_.late_frames += safe_frame - write_frame_;
        write_frame_ = safe_frame;
    }
    if (write_frame_ > played_frames + maximum_lead_frames_) {
        ++diagnostics_.dropped_packets;
        return false;
    }
    const std::uint64_t packet_start = write_frame_;
    const bool fade_in = !playing_ || packet_start > previous_end;
    // ARM11 has no hardware integer divider. Reduce the absolute 64-bit
    // timeline once, then walk a native-width cursor instead of invoking
    // __aeabi_uldivmod for every PCM frame.
    auto ring_index = static_cast<std::size_t>(packet_start % ring_frames_);
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        // For this timer, output_frames is 1600 or 1601. The rare extra
        // sample lies halfway between two original samples, preserving every
        // native source sample and continuity at packet boundaries.
        const bool inserted = output_frames == packet_frames_ + 1U
            && frame == packet_frames_ / 2U;
        const auto source_frame = output_frames == packet_frames_ + 1U
            && frame > packet_frames_ / 2U ? frame - 1U : frame;
        const auto left = inserted
            ? (static_cast<std::int32_t>(
                interleaved[(source_frame - 1U) * 2U])
                + interleaved[source_frame * 2U]) / 2
            : static_cast<std::int32_t>(interleaved[source_frame * 2U]);
        const auto right = inserted
            ? (static_cast<std::int32_t>(
                interleaved[(source_frame - 1U) * 2U + 1U])
                + interleaved[source_frame * 2U + 1U]) / 2
            : static_cast<std::int32_t>(interleaved[source_frame * 2U + 1U]);
        // Smooth the first millisecond after an underrun. Contiguous packets
        // retain their exact SPC samples across their common boundary.
        const std::int32_t gain = fade_in && frame < 32U
            ? static_cast<std::int32_t>(frame) : 32;
        pcm_left_[ring_index] = static_cast<std::int16_t>(
            left * gain * static_cast<std::int32_t>(master_volume_) / 3200);
        pcm_right_[ring_index] = static_cast<std::int16_t>(
            right * gain * static_cast<std::int32_t>(master_volume_) / 3200);
        if (++ring_index == ring_frames_) ring_index = 0;
    }
    write_frame_ += output_frames;
    // Clear newly exposed future space so a short producer stall fades into
    // silence. Long synchronous SD operations must call stop_playback() first.
    const auto clear_begin = std::max(silence_until_frame_, write_frame_);
    const auto clear_end = write_frame_ + silence_ahead_frames_;
    if (clear_begin < clear_end) {
        auto offset = static_cast<std::size_t>(clear_begin % ring_frames_);
        auto remaining = clear_end - clear_begin;
        while (remaining != 0) {
            const auto frames = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, ring_frames_ - offset));
            std::memset(pcm_left_ + offset, 0, frames * sizeof(std::int16_t));
            std::memset(pcm_right_ + offset, 0, frames * sizeof(std::int16_t));
            remaining -= frames;
            offset = 0;
        }
    }
    silence_until_frame_ = std::max(silence_until_frame_, clear_end);
    // If playback reaches the end of newly generated PCM, the 32-frame tail
    // decays to silence rather than making a step. The next packet overwrites
    // this tail when it arrives in time.
    // The packet cursor already points immediately after the new samples.
    for (std::size_t frame = 0; frame < 32U; ++frame) {
        const auto gain = static_cast<std::int32_t>(31U - frame);
        pcm_left_[ring_index] = static_cast<std::int16_t>(
            interleaved[(packet_frames_ - 1U) * 2U] * gain / 32);
        pcm_right_[ring_index] = static_cast<std::int16_t>(
            interleaved[(packet_frames_ - 1U) * 2U + 1U] * gain / 32);
        if (++ring_index == ring_frames_) ring_index = 0;
    }
    const auto flush_span = [&](std::int16_t* pcm, std::uint64_t begin,
                                std::uint64_t end) {
        while (begin < end) {
            const auto offset = begin % ring_frames_;
            const auto frames = std::min<std::uint64_t>(end - begin,
                ring_frames_ - offset);
            if (R_FAILED(GSPGPU_FlushDataCache(pcm + offset,
                static_cast<u32>(frames * sizeof(std::int16_t)))))
                return false;
            begin += frames;
        }
        return true;
    };
    // The first packet after a start zeroed both rings in full above, so it
    // flushes them in full. Later packets flush only the packet, its 32-frame
    // tail and the newly cleared future segment.
    const bool flushed = !playing_
        ? R_SUCCEEDED(GSPGPU_FlushDataCache(pcm_left_, ring_bytes_))
            && R_SUCCEEDED(GSPGPU_FlushDataCache(pcm_right_, ring_bytes_))
        : flush_span(pcm_left_, packet_start, write_frame_ + 32U)
            && flush_span(pcm_right_, packet_start, write_frame_ + 32U)
            && flush_span(pcm_left_, clear_begin, clear_end)
            && flush_span(pcm_right_, clear_begin, clear_end);
    if (!flushed) {
        ++diagnostics_.cache_flush_failures;
        return false;
    }
    ++diagnostics_.submitted_packets;
    diagnostics_.max_lead_frames = std::max(diagnostics_.max_lead_frames,
        static_cast<std::uint32_t>(write_frame_ - played_frames));
    if (playing_) return true;
    constexpr auto flags = SOUND_REPEAT | SOUND_FORMAT_16BIT
        | SOUND_LINEAR_INTERP;
    const Result left_result = csndPlaySound(left_channel_, flags,
        audio::Spc700Audio::sample_rate, 1.0F, -1.0F,
        pcm_left_, pcm_left_, ring_bytes_);
    const Result right_result = csndPlaySound(right_channel_, flags,
        audio::Spc700Audio::sample_rate, 1.0F, 1.0F,
        pcm_right_, pcm_right_, ring_bytes_);
    // csndPlaySound can return the positive value 1 when a channel is not
    // allocated, so a signed R_FAILED check would incorrectly accept it.
    if (left_result != 0 || right_result != 0) {
        CSND_SetPlayState(left_channel_, 0);
        CSND_SetPlayState(right_channel_, 0);
        static_cast<void>(CSND_UpdateInfo(true));
        diagnostics_.playback_start_result = left_result != 0
            ? left_result : right_result;
        return false;
    }
    playback_started_ms_ = osGetTime();
    diagnostics_.playback_start_result = 0;
    ++diagnostics_.playback_starts;
    playing_ = true;
    return true;
}

} // namespace starfox::platform_3ds
