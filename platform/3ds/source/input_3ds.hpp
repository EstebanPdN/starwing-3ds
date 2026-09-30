#pragma once

#include "starfox/input/input_latch.hpp"

namespace starfox::platform_3ds {

// Poll once per displayed frame (60 Hz), then consume once per simulation
// tick (20 Hz). InputLatch retains taps that begin and end between ticks.
class Input3ds {
public:
    void sample_display_frame() noexcept;
    void reserve_select_for_overlay(bool value) noexcept { reserve_select_=value; }
    [[nodiscard]] input::TickInput consume_game_tick() noexcept {
        return latch_.consume();
    }
    void reset() noexcept { latch_.reset(); }
    void suppress_until_release(unsigned keys) noexcept { blocked_keys_|=keys; latch_.reset(); }

private:
    input::InputLatch latch_;
    bool reserve_select_{};
    unsigned blocked_keys_{};
};

} // namespace starfox::platform_3ds
