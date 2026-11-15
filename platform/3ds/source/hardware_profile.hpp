#pragma once
// Per-model defaults chosen once at startup from APT_CheckNew3DS. Used by
// game_3ds.cpp and options_menu_3ds.cpp; host tests pin the values.
namespace starfox::platform_3ds {
// old_3ds selects Old 3DS behaviour, including separate options and autosave
// files. original_speed, interpolate and default_fps_limit (presented frames
// per second) are Options defaults that a saved settings file overrides.
// pica_world lets world geometry use the PICA200 native engine when it opens;
// name labels logs and diagnostics.
struct HardwareProfile {
    bool old_3ds;
    bool original_speed;
    bool interpolate;
    unsigned default_fps_limit;
    bool pica_world;
    const char* name;
};
constexpr HardwareProfile hardware_profile(bool new_3ds) noexcept {
    return new_3ds ? HardwareProfile{false, true, true, 60U, true, "New 3DS"}
                   : HardwareProfile{true, true, true, 30U, true, "Old 3DS"};
}
} // namespace starfox::platform_3ds
