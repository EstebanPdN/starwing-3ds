#pragma once

#include "starfox/render/framebuffer.hpp"
#include "starfox/simulation/snes_ppu.hpp"

#include <cstdint>
#include <span>

namespace starfox::render {

// Center-height left wall, including source HDMA scroll. Transparent wall
// texels fall back to the darkest palette entry, never the backdrop color.
[[nodiscard]] std::uint8_t tunnel_wall_index(
    const simulation::SnesPpuState& ppu) noexcept;

enum class TilePriorityPass {
    all,
    low,
    high,
};

// Authored non-repeating artwork embedded in an otherwise repeating tilemap.
// Only matching indexed pixels outside the native window are replaced.
struct BackgroundUniqueRegion {
    std::int32_t left, top, right, bottom;
    std::uint8_t first_colour, last_colour, replacement_colour;
};

class BackgroundRenderer {
public:
    void draw_bg1(
        const simulation::SnesPpuState& ppu,
        Framebuffer& target,
        TilePriorityPass priority = TilePriorityPass::all,
        std::int32_t horizontal_origin = 0,
        bool extend_horizontal = true,
        std::uint32_t horizontal_inset = 0,
        bool transparent_cgram_black = false) const noexcept;
    void draw_bg2(
        const simulation::SnesPpuState& ppu,
        std::int32_t scroll_x,
        std::int32_t scroll_y,
        Framebuffer& target,
        TilePriorityPass priority = TilePriorityPass::all,
        std::int32_t horizontal_origin = 0,
        bool extend_horizontal = true,
        bool wrap_horizontal = true,
        bool transparent_cgram_black = false,
        std::uint32_t single_occurrence_top_rows = 0U,
        std::span<const BackgroundUniqueRegion> unique_regions = {},
        std::int32_t vertical_origin = 0) const noexcept;
    // Mode 1's two priority passes share a single native-size tile traversal.
    // The caller supplies separate, already-cleared destinations and composites
    // them in SNES priority order. Other sizes use the ordinary two passes.
    void draw_bg2_mode1_split(
        const simulation::SnesPpuState& ppu,
        std::int32_t scroll_x,
        std::int32_t scroll_y,
        Framebuffer& low,
        Framebuffer& high) const noexcept;
    void draw_bg3(
        const simulation::SnesPpuState& ppu,
        Framebuffer& target,
        TilePriorityPass priority = TilePriorityPass::all,
        std::int32_t horizontal_origin = 0,
        bool extend_horizontal = true) const noexcept;
    void draw_bg3_split(
        const simulation::SnesPpuState& ppu,
        Framebuffer& low,
        Framebuffer& high) const noexcept;
    void draw_title_foreground(
        const simulation::SnesPpuState& ppu,
        std::int32_t bg2_scroll_x,
        std::int32_t bg2_scroll_y,
        Framebuffer& target,
        std::int32_t horizontal_origin = 0,
        bool include_bg1_overlay = true,
        bool extend_bg2_unwrapped = false) const noexcept;
};

} // namespace starfox::render
