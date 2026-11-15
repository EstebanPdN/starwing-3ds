#pragma once

#include "starfox/input/input_latch.hpp"

// 3DS buttons and Circle Pad mapped to the game's SNES button mask, with the
// frontend's own chords kept away from the game. Used by game_3ds.cpp.

namespace starfox::platform_3ds {

// Poll once per displayed frame (60 Hz), then consume once per simulation
// tick (20 Hz). InputLatch retains taps that begin and end between ticks.
class Input3ds {
public:
    // Scans HID and latches the mapped buttons; drops blocked keys and chords.
    void sample_display_frame() noexcept;
    // While set, SELECT is withheld from the game (game_3ds.cpp uses it to toggle
    // the diagnostic overlay).
    void reserve_select_for_overlay(bool value) noexcept { reserve_select_=value; }
    [[nodiscard]] input::TickInput consume_game_tick() noexcept {
        return latch_.consume();
    }
    void reset() noexcept { latch_.reset(); }
    // Hides the given KEY_* bits from the game until each one is released and
    // clears latched input, so buttons that closed a menu do not reach the game.
    void suppress_until_release(unsigned keys) noexcept { blocked_keys_|=keys; latch_.reset(); }

private:
    input::InputLatch latch_;
    bool reserve_select_{};
    unsigned blocked_keys_{};
};

} // namespace starfox::platform_3ds
