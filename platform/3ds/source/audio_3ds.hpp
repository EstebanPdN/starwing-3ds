#pragma once

#include "starfox/audio/spc700_audio.hpp"

#include <3ds.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
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
        // SPC compute time excludes waiting for the worker and CSND upload.
        std::uint64_t spc_compute_total_ms{};
        std::uint32_t spc_compute_last_ms{};
        std::uint32_t spc_compute_max_ms{};
        std::uint32_t spc_compute_packets{};
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

    // Feed the complete cartridge boot APU upload before the first logic tick.
    [[nodiscard]] std::size_t prime_upload_sequence(
        std::span<const simulation::ApuPortWrite> writes);

    // Always advances the SPC, including when CSND is unavailable or an old
    // playback packet must be dropped. Returns whether output was queued.
    [[nodiscard]] bool update(
        std::span<const simulation::ApuPortWrite> writes);

    // Copy the completed 20 Hz tick's port writes. New 3DS normally computes
    // audio on core 2 while video is built; Old 3DS computes it synchronously
    // on the application core to avoid its quota-limited core 1. A failed
    // New 3DS core-2 launch may use core 1. finish_update() must be called
    // before reading output_ports() or starting the next game tick.
    void begin_update(std::span<const simulation::ApuPortWrite> writes);
    [[nodiscard]] bool finish_update();
    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] bool update_ready() const noexcept {
        return pending_ && (worker_ == nullptr
            || worker_completed_.load(std::memory_order_acquire));
    }

    // Append one 50 ms packet to the continuously playing CSND ring.
    [[nodiscard]] bool push_samples(std::span<const std::int16_t> interleaved);
    void set_master_volume(unsigned percent) noexcept {
        master_volume_ = percent > 100U ? 100U : percent;
    }

    [[nodiscard]] std::array<std::uint8_t, 4> output_ports() const noexcept {
        return spc_.output_ports();
    }
    [[nodiscard]] audio::Spc700Audio& spc() noexcept { return spc_; }
    [[nodiscard]] const audio::Spc700Audio& spc() const noexcept { return spc_; }

private:
    static void worker_main(void* argument);
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
    std::atomic<bool> stop_worker_{false};
    std::atomic<bool> worker_completed_{false};
    // Written by the worker before its completion event, then read by the
    // application thread only after that event has been acquired.
    std::uint32_t worker_compute_ms_{};
    std::vector<simulation::ApuPortWrite> pending_writes_;
    std::vector<std::int16_t> rendered_;
    std::exception_ptr worker_failure_{};
    bool pending_{};
    bool quota_changed_{};
    AsyncDiagnostics diagnostics_{};
};

} // namespace starfox::platform_3ds
