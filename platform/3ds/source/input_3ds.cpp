#include "input_3ds.hpp"

#include "starfox/input/buttons.hpp"

#include <3ds.h>

namespace starfox::platform_3ds {
namespace {

constexpr int kCircleThreshold = 48;

void add_button(input::ButtonMask& result, u32 held, u32 physical,
                input::Button native) noexcept {
    if (held & physical) {
        result = static_cast<input::ButtonMask>(result | native);
    }
}

} // namespace

void Input3ds::sample_display_frame() noexcept {
    hidScanInput();
    // Include a newly pressed button even when it was released before the
    // next simulation tick; InputLatch carries that tap to the game.
    u32 held = hidKeysHeld() | hidKeysDown();
    blocked_keys_ &= held;
    held &= ~blocked_keys_;
    if ((held & (KEY_L | KEY_R | KEY_SELECT))
        == (KEY_L | KEY_R | KEY_SELECT)) {
        // The global diagnostic chord belongs to the 3DS UI, regardless of
        // the in-game SELECT overlay preference or the current scene.
        held &= ~(KEY_L | KEY_R | KEY_SELECT);
    } else if (reserve_select_) {
        held &= ~KEY_SELECT;
    }
    if ((held & (KEY_L | KEY_R | KEY_X))
        == (KEY_L | KEY_R | KEY_X)) {
        held &= ~(KEY_L | KEY_R | KEY_X);
    } else if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R)
        && (held & (KEY_A | KEY_B)) != 0U) {
        held &= ~(KEY_L | KEY_R | KEY_A | KEY_B);
    }
    circlePosition circle{};
    hidCircleRead(&circle);

    input::ButtonMask buttons{};
    add_button(buttons, held, KEY_A, input::a);
    add_button(buttons, held, KEY_B, input::b);
    add_button(buttons, held, KEY_X, input::x);
    add_button(buttons, held, KEY_Y, input::y);
    add_button(buttons, held, KEY_L, input::left_shoulder);
    add_button(buttons, held, KEY_R, input::right_shoulder);
    add_button(buttons, held, KEY_START, input::start);
    add_button(buttons, held, KEY_SELECT, input::select);

    // The D-pad takes priority on each axis; Circle Pad supplies steering
    // when that axis has no digital direction held.
    const bool horizontal_dpad = (held & (KEY_DLEFT | KEY_DRIGHT)) != 0;
    const bool vertical_dpad = (held & (KEY_DUP | KEY_DDOWN)) != 0;
    if (horizontal_dpad) {
        add_button(buttons, held, KEY_DLEFT, input::left);
        add_button(buttons, held, KEY_DRIGHT, input::right);
    } else {
        if (circle.dx < -kCircleThreshold) buttons |= input::left;
        if (circle.dx > kCircleThreshold) buttons |= input::right;
    }
    if (vertical_dpad) {
        add_button(buttons, held, KEY_DUP, input::up);
        add_button(buttons, held, KEY_DDOWN, input::down);
    } else {
        if (circle.dy > kCircleThreshold) buttons |= input::up;
        if (circle.dy < -kCircleThreshold) buttons |= input::down;
    }
    latch_.sample(buttons);
}

} // namespace starfox::platform_3ds
