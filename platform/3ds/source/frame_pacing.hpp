#pragma once
#include <algorithm>
#include <cstdint>

// FramePacing: 60 Hz presentation clock in hardware-tick units. Used by game_3ds.cpp;
// host-tested by tests/frame_pacing_test.cpp.

namespace starfox::platform_3ds {
// Rational 60 Hz clock: one phase costs ticks_per_second units, while each
// elapsed hardware tick contributes 60 units. No millisecond quantization or
// rounded 16667-us step can put an otherwise due phase past its deadline.
class FramePacing {
public:
    // One 20 Hz source/audio period per presentation. A late caller retains
    // the remaining debt, but presents before another packet and source tick
    // can serially block the same image. Light frames still consume one phase.
    static constexpr unsigned max_video_phases_per_presentation = 3U;
    constexpr explicit FramePacing(std::uint64_t ticks_per_second)
        : frequency_(ticks_per_second) {}
    constexpr void reset() noexcept { debt_ = 0U; }
    // Adds elapsed hardware ticks. One call counts at most 100 ms and the
    // debt is capped at 12 phases, so a stall or suspend cannot cause a long
    // catch-up burst.
    constexpr void advance(std::uint64_t elapsed_ticks) noexcept {
        debt_ = std::min(debt_ + std::min(elapsed_ticks, frequency_ / 10U) * 60U,
            frequency_ * 12U);
    }
    // ready(): at least one phase is due; consume() takes one due phase.
    [[nodiscard]] constexpr bool ready() const noexcept { return debt_ >= frequency_; }
    constexpr void consume() noexcept { if (ready()) debt_ -= frequency_; }
    // Hardware ticks (or nanoseconds, rounded up) until the next phase is
    // due, given the ticks elapsed since the last advance().
    [[nodiscard]] constexpr std::uint64_t remaining_ticks(
        std::uint64_t elapsed_since_sample) const noexcept {
        if (ready()) return 0U;
        // Round the phase boundary up to a hardware tick, and never multiply
        // the unbounded elapsed value supplied by a resumed caller.
        const auto needed = (frequency_ - debt_ + 59U) / 60U;
        return elapsed_since_sample >= needed ? 0U : needed - elapsed_since_sample;
    }
    [[nodiscard]] constexpr std::uint64_t remaining_nanoseconds(
        std::uint64_t elapsed_since_sample) const noexcept {
        // A remaining phase is at most ~16.7 ms. At the ARM11 clock this
        // bounded product fits in 64 bits, including after a long suspend.
        const auto scaled = remaining_ticks(elapsed_since_sample) * 1'000'000'000U;
        return scaled / frequency_ + (scaled % frequency_ != 0U);
    }
    [[nodiscard]] constexpr bool should_wait(std::uint64_t elapsed_since_sample) const noexcept {
        return remaining_ticks(elapsed_since_sample) != 0U;
    }
private:
    std::uint64_t frequency_;
    std::uint64_t debt_{};
};
} // namespace starfox::platform_3ds
