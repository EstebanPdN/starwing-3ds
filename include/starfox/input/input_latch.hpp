#pragma once

#include <cstdint>

// Per-tick controller input: ButtonMask (Button bits from buttons.hpp, in the
// SNES joypad layout) and TickInput, which GameSimulation::tick consumes once
// per 20 Hz tick.
namespace starfox::input {

using ButtonMask = std::uint16_t;

// Buttons held at the tick, and the buttons newly pressed or released since
// the previous tick.
struct TickInput {
    ButtonMask held{};
    ButtonMask pressed{};
    ButtonMask released{};
};

// Presentation may poll the controller several times per gameplay tick (three
// at 60 Hz). This latch retains transitions until the 20 Hz simulation
// consumes them: consume() returns the latest held state with every press and
// release since the last consume() or reset(), then clears those edges.
class InputLatch {
public:
    void sample(ButtonMask held) noexcept {
        pressed_ |= static_cast<ButtonMask>(held & ~last_sample_);
        released_ |= static_cast<ButtonMask>(last_sample_ & ~held);
        held_ = held;
        last_sample_ = held;
    }

    [[nodiscard]] TickInput consume() noexcept {
        const TickInput result{held_, pressed_, released_};
        pressed_ = 0;
        released_ = 0;
        return result;
    }

    void reset(ButtonMask held = 0) noexcept {
        held_ = held;
        last_sample_ = held;
        pressed_ = 0;
        released_ = 0;
    }

private:
    ButtonMask held_{};
    ButtonMask last_sample_{};
    ButtonMask pressed_{};
    ButtonMask released_{};
};

} // namespace starfox::input

