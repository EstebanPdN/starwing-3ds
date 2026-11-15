#pragma once

#include "starfox/audio/spc700_audio.hpp"

#include <3ds.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <span>
#include <vector>

namespace starfox::platform_3ds {

// CSND consumes the native SPC output as two signed 16-bit mono channels at
// 32 kHz. Call update() once per 20 Hz simulation logic tick.
class Audio3ds {
public:
    struct AsyncDiagnostics {
        Result csnd_result{-1};
        Result model_result{-1};
        Result quota_query_result{-1};
        Result quota_set_result{-1};
        Result quota_verify_result{-1};
        Result quota_restore_result{-1};
        bool new_3ds{};
        bool core2_attempted{};
        bool core2_created{};
        unsigned core1_attempts{};
        int selected_core{-1};
        u32 original_quota{};
        u32 effective_quota{};
        // CSND playback is an independently clocked, continuously looping
        // PCM ring. These counters distinguish producer starvation from a
        // graphics stall or a failed sound-service call.
        Result playback_start_result{-1};
        std::uint32_t submitted_packets{};
        std::uint32_t late_packets{};
        std::uint64_t late_frames{};
        std::uint32_t dropped_packets{};
        std::uint32_t playback_starts{};
        std::uint32_t cache_flush_failures{};
        std::uint32_t max_lead_frames{};
        std::uint64_t rate_adjust_frames{};
        // Elapsed time inside SPC rendering, excluding the caller's wait and
        // CSND upload. Includes preemption by another thread on this core;
        // it is not an isolated CPU-time measurement.
        std::uint64_t spc_compute_total_ms{};
        std::uint32_t spc_compute_last_ms{};
        std::uint32_t spc_compute_max_ms{};
        std::uint32_t spc_compute_packets{};
        std::uint32_t spc_compute_last_us{}, worker_queue_last_us{}, wait_last_us{};
        std::uint32_t ports_ready_last_us{}, ports_wait_last_us{};
        std::uint64_t wait_total_us{};
    };

    Audio3ds();
    ~Audio3ds();
    Audio3ds(const Audio3ds&) = delete;
    Audio3ds& operator=(const Audio3ds&) = delete;

    [[nodiscard]] bool open();
    void close() noexcept;
    // Stop the hardware reader during long, synchronous SD operations. The
    // SPC state is retained; the next completed logic tick starts a fresh
    // playback ring after the operation returns.
    void stop_playback() noexcept;
    // Before a HOME or POWER transition: make the looping rings silent
    // without issuing a CSND command. libctru's completion wait is an
    // unbounded spin, so nothing here may depend on the sound service
    // answering. The channels keep running; stop_playback() after resuming
    // lets the next tick start a fresh ring.
    void silence_for_suspend() noexcept;
    // After returning from HOME or sleep, and when closing: submit the stop
    // without spinning on CSND's completion flag. An application woken only
    // to close must not depend on the sound service answering.
    void stop_playback_no_wait() noexcept { stop_playback(false); }
    // The system can drop the core-1 share while the application is
    // suspended. Ask again for the share that was granted at open().
    void reapply_cpu_quota() noexcept;
    // Power-on SPC for a freshly constructed simulation (the test tour
    // enters levels directly). The caller then primes the boot upload.
    void restart_spc();
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] bool async_enabled() const noexcept {
        return worker_ != nullptr;
    }
    [[nodiscard]] int effective_core() const noexcept {
        return diagnostics_.selected_core;
    }
    [[nodiscard]] int worker_core() const noexcept { return effective_core(); }
    [[nodiscard]] u32 effective_quota() const noexcept {
        return diagnostics_.effective_quota;
    }
    [[nodiscard]] u32 cpu_quota_percent() const noexcept {
        return effective_quota();
    }
    [[nodiscard]] const AsyncDiagnostics& diagnostics() const noexcept {
        return diagnostics_;
    }
    // Stable main-thread ownership; excludes the worker-mutated SPC buffers.
    [[nodiscard]] std::size_t pcm_ring_bytes() const noexcept {
        return (std::size_t(pcm_left_ != nullptr) + std::size_t(pcm_right_ != nullptr)) * ring_bytes_;
    }

    // Feed the complete cartridge boot APU upload before the first logic tick.
    [[nodiscard]] std::size_t prime_upload_sequence(
        std::span<const simulation::ApuPortWrite> writes);

    // Always advances the SPC, including when CSND is unavailable or an old
    // playback packet must be dropped. Returns whether output was queued.
    [[nodiscard]] bool update(
        std::span<const simulation::ApuPortWrite> writes);

    // Hand over the port writes of one 50 ms packet. New 3DS normally
    // computes audio on core 2 while video is built; Old 3DS uses core 1 when
    // APT grants at least a 50% slice, else the application core. A failed
    // New 3DS core-2 launch may use core 1.
    //
    // Packets queue: the worker renders them one after another and the caller
    // never waits for one to start the next (it used to, a whole packet's
    // worth each time two fell due together). A game tick that reads the
    // sound ports calls wait_for_output_ports(), which waits for the ports of
    // the last packet handed over, exactly the values an eager wait gave.
    // collect() publishes the PCM of the packets that are done;
    // finish_update() waits for all of them first and must precede direct
    // access to the SPC state.
    void begin_update(std::span<const simulation::ApuPortWrite> writes);
    [[nodiscard]] bool finish_update();
    // Never waits. True when it published at least one packet's ports and PCM.
    bool collect();
    [[nodiscard]] std::array<std::uint8_t,4> wait_for_output_ports();
    [[nodiscard]] bool pending() const noexcept { return submitted_ != collected_; }
    // Everything handed over is rendered and waits to be collected.
    [[nodiscard]] bool update_ready() const noexcept {
        return pending() && (worker_ == nullptr
            || completed_.load(std::memory_order_acquire) == submitted_);
    }

    // Append one 50 ms packet to the continuously playing CSND ring.
    [[nodiscard]] bool push_samples(std::span<const std::int16_t> interleaved);
    void set_master_volume(unsigned percent) noexcept {
        master_volume_ = percent > 100U ? 100U : percent;
    }

    // The newest ports this thread has taken from the worker; the SPC's own
    // when nothing is outstanding.
    [[nodiscard]] std::array<std::uint8_t, 4> output_ports() const noexcept {
        if (pending() && worker_ != nullptr) return known_ports_;
        return spc_.output_ports();
    }
    [[nodiscard]] audio::Spc700Audio& spc() noexcept { return spc_; }
    [[nodiscard]] const audio::Spc700Audio& spc() const noexcept { return spc_; }

private:
    void stop_playback(bool wait_done) noexcept;
    static void worker_main(void* argument);
    static void publish_ports(void*, const std::array<std::uint8_t,4>&) noexcept;
    // Publishes one rendered packet; `count` is its number plus one.
    bool collect_packet(std::uint32_t count);
    // Try each APT slice in order and start the worker on core 1.
    void start_core1_worker(std::initializer_list<u32> quotas);
    static constexpr int left_channel_ = 8;
    static constexpr int right_channel_ = 9;
    static constexpr std::size_t packet_frames_ =
        audio::Spc700Audio::stereo_frames_per_logic_tick;
    static constexpr std::size_t packet_samples_ = packet_frames_ * 2;
    static constexpr std::size_t ring_frames_ =
        audio::Spc700Audio::sample_rate * 2U;
    static constexpr std::size_t ring_bytes_ =
        ring_frames_ * sizeof(std::int16_t);
    static constexpr std::size_t initial_lead_frames_ =
        audio::Spc700Audio::sample_rate / 10U; // 100 ms
    static constexpr std::size_t write_guard_frames_ =
        audio::Spc700Audio::sample_rate / 50U; // 20 ms
    static constexpr std::size_t silence_ahead_frames_ =
        audio::Spc700Audio::sample_rate / 4U; // 250 ms
    static constexpr std::size_t maximum_lead_frames_ =
        audio::Spc700Audio::sample_rate / 2U; // 500 ms

    audio::Spc700Audio spc_;
    std::int16_t* pcm_left_{};
    std::int16_t* pcm_right_{};
    std::uint64_t playback_started_ms_{};
    std::uint64_t write_frame_{};
    std::uint64_t silence_until_frame_{};
    std::uint64_t source_frames_total_{};
    std::uint64_t output_frames_total_{};
    bool playing_{};
    unsigned master_volume_{100U};
    bool ready_{};
    Thread worker_{};
    LightEvent work_requested_{};
    LightEvent work_completed_{};
    LightEvent ports_completed_{};
    std::atomic<bool> stop_worker_{false};
    // One 50 ms packet on its way through the worker. The application thread
    // fills `writes` before it counts the packet in submitted_; the worker
    // owns the rest until completed_ (ports and ports_failure until ported_)
    // passes the packet.
    struct Packet {
        std::vector<simulation::ApuPortWrite> writes;
        std::vector<std::int16_t> pcm;
        std::array<std::uint8_t,4> ports{};
        std::exception_ptr failure{}, ports_failure{};
        std::uint64_t request_us{};
        std::uint32_t compute_ms{}, compute_us{}, queue_us{}, ports_ready_us{};
    };
    static constexpr std::uint32_t queue_slots_ = 4U;
    std::array<Packet, queue_slots_> packets_;
    // Packets handed over (application thread), packets whose ports are out
    // and packets fully rendered (worker), packets published (application
    // thread). Each counts from zero and never goes back.
    std::uint32_t submitted_{};
    std::atomic<std::uint32_t> handed_{0}, ported_{0}, completed_{0};
    std::uint32_t collected_{};
    std::array<std::uint8_t,4> known_ports_{};
    bool quota_changed_{};
    AsyncDiagnostics diagnostics_{};
};

} // namespace starfox::platform_3ds
