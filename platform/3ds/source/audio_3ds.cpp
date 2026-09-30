#include "audio_3ds.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace starfox::platform_3ds {

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

    if (R_SUCCEEDED(diagnostics_.model_result)
        && !diagnostics_.new_3ds) {
        // Old 3DS only grants this title a 30% core-1 slice. A 50 ms SPC
        // packet then holds the next simulation tick for roughly 170 ms.
        // Run the unchanged SPC tick on the application core instead.
        diagnostics_.selected_core = 0;
    } else if (worker_ == nullptr) {
        // If New 3DS core 2 was unavailable (or model detection failed),
        // retain the verified core-1 fallback and restore the prior quota.
        diagnostics_.quota_query_result =
            APT_GetAppCpuTimeLimit(&diagnostics_.original_quota);
        if (R_SUCCEEDED(diagnostics_.quota_query_result)) {
            diagnostics_.effective_quota = diagnostics_.original_quota;
            for (const u32 requested : {30U, 20U, 10U}) {
                ++diagnostics_.core1_attempts;
                diagnostics_.quota_set_result =
                    APT_SetAppCpuTimeLimit(requested);
                if (R_FAILED(diagnostics_.quota_set_result)) continue;
                quota_changed_ = true;
                u32 granted = 0;
                diagnostics_.quota_verify_result =
                    APT_GetAppCpuTimeLimit(&granted);
                if (R_SUCCEEDED(diagnostics_.quota_verify_result)
                    && granted == requested) {
                    diagnostics_.effective_quota = granted;
                    worker_ = threadCreate(&Audio3ds::worker_main, this,
                        128U * 1024U, 0x30, 1, false);
                    if (worker_ != nullptr) {
                        diagnostics_.selected_core = 1;
                        break;
                    }
                }
                diagnostics_.quota_restore_result =
                    APT_SetAppCpuTimeLimit(diagnostics_.original_quota);
                if (R_FAILED(diagnostics_.quota_restore_result)) break;
                quota_changed_ = false;
                diagnostics_.effective_quota = diagnostics_.original_quota;
            }
        }
        if (worker_ == nullptr) diagnostics_.selected_core = 0;
    }
    return ready_;
}

void Audio3ds::close() noexcept {
    if (worker_ != nullptr) {
        if (pending_) {
            LightEvent_Wait(&work_completed_);
            pending_ = false;
        }
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
    pending_ = false;
    pending_writes_.clear();
    rendered_.clear();
    worker_failure_ = nullptr;
    stop_playback();
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
    if (playing_) {
        CSND_SetPlayState(left_channel_, 0);
        CSND_SetPlayState(right_channel_, 0);
        static_cast<void>(CSND_UpdateInfo(true));
    }
    playing_ = false;
    playback_started_ms_ = 0;
    write_frame_ = 0;
    silence_until_frame_ = 0;
    source_frames_total_ = 0;
    output_frames_total_ = 0;
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
        std::atomic_thread_fence(std::memory_order_acquire);
        if (self.stop_worker_.load(std::memory_order_acquire)) break;
        try {
            const auto begin_ms = osGetTime();
            self.rendered_ = self.spc_.render_logic_tick(self.pending_writes_);
            const auto elapsed_ms = static_cast<std::uint32_t>(
                osGetTime() - begin_ms);
            self.worker_compute_ms_ = elapsed_ms;
        } catch (...) {
            self.worker_failure_ = std::current_exception();
        }
        std::atomic_thread_fence(std::memory_order_release);
        self.worker_completed_.store(true, std::memory_order_release);
        LightEvent_Signal(&self.work_completed_);
    }
}

void Audio3ds::begin_update(
    std::span<const simulation::ApuPortWrite> writes) {
    if (pending_) throw std::logic_error{"SPC update already pending"};
    if (worker_ == nullptr) {
        const auto begin_ms = osGetTime();
        rendered_ = spc_.render_logic_tick(writes);
        const auto elapsed_ms = static_cast<std::uint32_t>(
            osGetTime() - begin_ms);
        diagnostics_.spc_compute_last_ms = elapsed_ms;
        diagnostics_.spc_compute_total_ms += elapsed_ms;
        diagnostics_.spc_compute_max_ms = std::max(
            diagnostics_.spc_compute_max_ms, elapsed_ms);
        ++diagnostics_.spc_compute_packets;
    } else {
        pending_writes_.assign(writes.begin(), writes.end());
        worker_failure_ = nullptr;
        worker_completed_.store(false, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        LightEvent_Signal(&work_requested_);
    }
    pending_ = true;
}

bool Audio3ds::finish_update() {
    if (!pending_) return false;
    if (worker_ != nullptr) {
        LightEvent_Wait(&work_completed_);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!worker_failure_) {
            diagnostics_.spc_compute_last_ms = worker_compute_ms_;
            diagnostics_.spc_compute_total_ms += worker_compute_ms_;
            diagnostics_.spc_compute_max_ms = std::max(
                diagnostics_.spc_compute_max_ms, worker_compute_ms_);
            ++diagnostics_.spc_compute_packets;
        }
    }
    pending_ = false;
    pending_writes_.clear();
    if (worker_failure_) std::rethrow_exception(worker_failure_);
    return push_samples(rendered_);
}

bool Audio3ds::push_samples(std::span<const std::int16_t> interleaved) {
    if (!ready_ || interleaved.size() != packet_samples_) return false;
    // CSND has no application-visible playback cursor. A two-second ring and
    // a 20 ms write guard keep CPU writes away from the estimated read head.
    // The hardware channel is started once; restarting a 50 ms one-shot on
    // each logic tick used to truncate or gap the waveform as frame time
    // varied. A missed producer deadline is recorded instead of hidden.
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
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        const auto ring_index = (packet_start + frame) % ring_frames_;
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
    }
    write_frame_ += output_frames;
    // Clear newly exposed future space so a short producer stall fades into
    // silence. Long synchronous SD operations must call stop_playback() first.
    const auto clear_begin = std::max(silence_until_frame_, write_frame_);
    const auto clear_end = write_frame_ + silence_ahead_frames_;
    for (auto frame = clear_begin; frame < clear_end; ++frame) {
        const auto ring_index = frame % ring_frames_;
        pcm_left_[ring_index] = 0;
        pcm_right_[ring_index] = 0;
    }
    silence_until_frame_ = std::max(silence_until_frame_, clear_end);
    // If playback reaches the end of newly generated PCM, the 32-frame tail
    // decays to silence rather than making a step. The next packet overwrites
    // this tail when it arrives in time.
    for (std::size_t frame = 0; frame < 32U; ++frame) {
        const auto ring_index = (write_frame_ + frame) % ring_frames_;
        const auto gain = static_cast<std::int32_t>(31U - frame);
        pcm_left_[ring_index] = static_cast<std::int16_t>(
            interleaved[(packet_frames_ - 1U) * 2U] * gain / 32);
        pcm_right_[ring_index] = static_cast<std::int16_t>(
            interleaved[(packet_frames_ - 1U) * 2U + 1U] * gain / 32);
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
    // Initial allocation was zeroed in full. Subsequent updates flush only
    // the packet plus the newly cleared future segment.
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
