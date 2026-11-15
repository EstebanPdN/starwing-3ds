#pragma once

#include "starfox/render/framebuffer.hpp"
#include "starfox/simulation/snes_ppu.hpp"

#include <cstdint>
#include <memory>
#include <span>

// SNES background layers BG1-BG3 drawn from a PPU snapshot into an indexed
// Framebuffer: the cartridge's 256x224 window plus the policies that fill a
// wider or taller presentation. Used by the 3DS frontend, host tools and tests.
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
// Bounds are half-open tilemap pixel coordinates; the colour range is inclusive.
struct BackgroundUniqueRegion {
    std::int32_t left, top, right, bottom;
    std::uint8_t first_colour, last_colour, replacement_colour;
};

class BackgroundRenderer {
public:
    BackgroundRenderer() noexcept;
    ~BackgroundRenderer();
    BackgroundRenderer(BackgroundRenderer&&) noexcept;
    BackgroundRenderer& operator=(BackgroundRenderer&&) noexcept;

    // The low and high priority passes of one frame decode the same
    // characters. Between begin_frame() and end_frame(), passes given this
    // exact PPU object (which must not change meanwhile) share decoded
    // characters; any other object, or a call outside a frame, decodes anew.
    void begin_frame(const simulation::SnesPpuState& ppu);
    void end_frame() noexcept;

    // BG1 in Modes 1-3 (Mode 3 is 8bpp with direct CGRAM indices, the others
    // 4bpp); draws nothing for other modes or when BG1 is off the main screen.
    // horizontal_origin places the 256-pixel cartridge window in a wider
    // target. Without extend_horizontal the layer stays inside that window,
    // narrowed by horizontal_inset (at most 128) on each side.
    // transparent_cgram_black skips pixels whose CGRAM colour is black.
    void draw_bg1(
        const simulation::SnesPpuState& ppu,
        Framebuffer& target,
        TilePriorityPass priority = TilePriorityPass::all,
        std::int32_t horizontal_origin = 0,
        bool extend_horizontal = true,
        std::uint32_t horizontal_inset = 0,
        bool transparent_cgram_black = false) const noexcept;
    // BG2 (4bpp); nothing when BG2 is off the main screen. scroll_x/scroll_y
    // apply where no HDMA line table or valid Mode 2 per-tile offset replaces
    // them. horizontal_origin/vertical_origin place the 256x224 cartridge
    // window in a larger target; with extend_horizontal the margins are drawn
    // too, using the wide-view policies in the implementation (tunnel walls,
    // water edges, Mode 2 horizon and ground continuation). wrap_horizontal
    // false draws a single tilemap occurrence. single_occurrence_top_rows and
    // unique_regions keep one-off authored art from repeating into the margins.
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
    // Mode 1's two priority passes share a single tile traversal.
    // The caller supplies separate, already-cleared destinations and composites
    // them in SNES priority order. An origin centres the cartridge window in a
    // wider raster; added vertical rows retain the nearest authored scanline.
    // Outside Mode 1, or for destinations of different sizes, it falls back
    // to two draw_bg2 passes.
    void draw_bg2_mode1_split(
        const simulation::SnesPpuState& ppu,
        std::int32_t scroll_x,
        std::int32_t scroll_y,
        Framebuffer& low,
        Framebuffer& high,
        std::int32_t horizontal_origin = 0,
        std::int32_t vertical_origin = 0) const noexcept;
    // BG3 (2bpp, CGRAM 0-31); nothing when BG3 is off the main screen.
    // horizontal_origin and extend_horizontal work as for draw_bg1.
    void draw_bg3(
        const simulation::SnesPpuState& ppu,
        Framebuffer& target,
        TilePriorityPass priority = TilePriorityPass::all,
        std::int32_t horizontal_origin = 0,
        bool extend_horizontal = true) const noexcept;
    // BG3 counterpart of draw_bg2_mode1_split, with the cartridge window at
    // x = 0 of both destinations.
    void draw_bg3_split(
        const simulation::SnesPpuState& ppu,
        Framebuffer& low,
        Framebuffer& high) const noexcept;
    // Title-screen layers that belong in front of the Super FX model: BG2's
    // high-priority tiles, BG1 when include_bg1_overlay is set, then
    // high-priority BG3. extend_bg2_unwrapped draws one unwrapped BG2
    // occurrence across the wide view and insets BG1 by 16 pixels.
    void draw_title_foreground(
        const simulation::SnesPpuState& ppu,
        std::int32_t bg2_scroll_x,
        std::int32_t bg2_scroll_y,
        Framebuffer& target,
        std::int32_t horizontal_origin = 0,
        bool include_bg1_overlay = true,
        bool extend_bg2_unwrapped = false) const noexcept;

    struct FrameCharacters;
private:
    std::unique_ptr<FrameCharacters> frame_characters_;
};

} // namespace starfox::render
