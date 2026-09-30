#pragma once
#include <algorithm>
#include <cstdint>

namespace starfox::platform_3ds {
// Rational 60 Hz clock: one phase costs ticks_per_second units, while each
// elapsed hardware tick contributes 60 units. No millisecond quantization or
// rounded 16667-us step can put an otherwise due phase past the next VBlank.
class FramePacing {
public:
    constexpr explicit FramePacing(std::uint64_t ticks_per_second)
        : frequency_(ticks_per_second) {}
    constexpr void reset() noexcept { debt_ = 0U; }
    constexpr void advance(std::uint64_t elapsed_ticks) noexcept {
        debt_ = std::min(debt_ + std::min(elapsed_ticks, frequency_ / 10U) * 60U,
            frequency_ * 12U);
    }
    [[nodiscard]] constexpr bool ready() const noexcept { return debt_ >= frequency_; }
    constexpr void consume() noexcept { if (ready()) debt_ -= frequency_; }
    [[nodiscard]] constexpr bool should_wait(std::uint64_t elapsed_since_sample) const noexcept {
        if (ready()) return false;
        // Compare without multiplying the unbounded caller's elapsed value.
        return elapsed_since_sample < (frequency_ - debt_ + 59U) / 60U;
    }
private:
    std::uint64_t frequency_;
    std::uint64_t debt_{};
};
} // namespace starfox::platform_3ds
