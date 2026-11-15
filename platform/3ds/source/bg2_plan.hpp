#pragma once

#include "starfox/simulation/snes_ppu.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// BG2 raster planning for the GPU path: plan_bg2_rects() splits native Mode 2 BG2 into source
// rectangles (Bg2Rect) that display_3ds draws from its tile atlas or predecoded map texture.
// Host-tested by tests/bg2_plan_test.cpp, bg2_atlas_cache_test.cpp and mode2_phase_test.cpp.

namespace starfox::platform_3ds {
class Bg2MapSourceCache;

// One unscaled source-raster fragment of an 8x8 4bpp BG2 character. The
// source coordinates already include SNES tile flips. A consumer walks them
// forward or backward according to reverse_x/reverse_y.
struct Bg2Rect {
    // Destination in target pixels. source_x/source_y give the first texel
    // inside the character, or inside the whole map with map_sample.
    std::uint16_t x{}, y{}, width{}, height{};
    std::uint16_t character{};
    std::uint8_t palette_bank{};
    std::uint16_t source_x{}, source_y{};
    bool reverse_x{}, reverse_y{};
    // Fill with the palette index solid_colour instead of sampling: runs of
    // single-colour opaque characters, and the carry colour that continues
    // rolled ground after its source tilemap wraps or a ground character
    // has a transparent pixel.
    bool solid{};
    std::uint8_t solid_colour{};
    // Coordinates in the predecoded BG2 tile map rather than one character.
    bool map_sample{};
    // Certified nearest-neighbour affine sampling of a rolled map row.
    // Q9 Y at the top-left vertex; Q8 change of Y per output X pixel.
    bool affine_map{};
    std::int32_t map_y_q9{}, map_dy_dx_q8{};
    friend constexpr bool operator==(const Bg2Rect&, const Bg2Rect&) = default;
};

// Plans a native Mode 2 BG2 raster. The SNES 256x224 viewport begins at
// (origin_x, origin_y); pixels outside it continue the BG2 tile map while
// line-specific HDMA uses the nearest source scanline. Returns false, with no
// partial output, for unsupported mosaic/mode, an empty or larger than
// 400x240 target, or more than `capacity` rectangles; returns true with no
// rectangles when BG2 is off the main screen. Colour-zero transparency is
// applied by the consumer's tile atlas. use_map_texture plans map_sample
// rectangles for the predecoded map texture. cache_rolled_ground_samples =
// false resolves the rolled-ground carry without sample caches, as the
// reference path.
[[nodiscard]] bool plan_bg2_rects(
    const simulation::SnesPpuState& ppu,
    std::int32_t scroll_x, std::int32_t scroll_y,
    std::vector<Bg2Rect>& output,
    std::size_t capacity = 14560U,
    std::uint32_t target_width = 256U,
    std::uint32_t target_height = 224U,
    std::int32_t origin_x = 0,
    std::int32_t origin_y = 0,
    bool cache_rolled_ground_samples = true,
    bool use_map_texture = false,
    // True only with a current Bg2MapSourceCache::all_opaque() certificate.
    bool map_is_opaque = false,
    const Bg2MapSourceCache* indexed_map = nullptr,
    // The simulation's VRAM revision for `ppu`, when the caller knows it:
    // lets the indexed map prove itself current without comparing its bytes.
    std::uint64_t ppu_vram_revision = 0U);

} // namespace starfox::platform_3ds
