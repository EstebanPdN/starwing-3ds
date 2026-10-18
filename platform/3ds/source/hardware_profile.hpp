#pragma once
namespace starfox::platform_3ds {
struct HardwareProfile {
    bool old_3ds;
    bool original_speed;
    bool interpolate;
    unsigned presentation_hz;
    const char* name;
};
constexpr HardwareProfile hardware_profile(bool new_3ds) noexcept {
    return new_3ds ? HardwareProfile{false, false, true, 60U, "New 3DS"}
                   : HardwareProfile{true, true, false, 30U, "Old 3DS"};
}
} // namespace starfox::platform_3ds
