#pragma once
namespace starfox::platform_3ds {
struct HardwareProfile {
    bool old_3ds;
    bool original_speed;
    bool interpolate;
    unsigned default_fps_limit;
    bool pica_world;
    const char* name;
};
constexpr HardwareProfile hardware_profile(bool new_3ds) noexcept {
    return new_3ds ? HardwareProfile{false, false, true, 0U, true, "New 3DS"}
                   : HardwareProfile{true, true, false, 0U, true, "Old 3DS"};
}
} // namespace starfox::platform_3ds
