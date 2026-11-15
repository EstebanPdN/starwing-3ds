#pragma once

#include <cstdint>

// Deterministic star placement for skies the cartridge does not draw: the
// added native columns (native_canvas.cpp) and the lower screen
// (options_menu_3ds.cpp).

namespace starfox::platform_3ds {

// The dump 022 cartridge sky has about one lit pixel per 420 black pixels.
// Keep the added native columns and lower screen at that same density.
// A 32-bit hash of the pixel position and seed; callers test its low 12 bits
// for a star and use the high bits for its shade.
constexpr std::uint32_t game_over_star_hash(unsigned x, unsigned y,
    std::uint32_t seed = 0U) noexcept {
    auto value = x * 0x9e3779b1U + y * 0x85ebca77U + seed;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    return value ^ (value >> 16U);
}

// A star at 10 of every 4096 positions.
constexpr bool game_over_star(unsigned x, unsigned y,
    std::uint32_t seed = 0U) noexcept {
    return (game_over_star_hash(x,y,seed) & 4095U) < 10U;
}

} // namespace starfox::platform_3ds
