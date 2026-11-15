#pragma once

#include "starfox/render/framebuffer.hpp"
#include "starfox/simulation/snes_ppu.hpp"

#include <span>

// The 400x240 native top-screen canvas built around the cartridge's 256x224
// picture, which keeps its pixels at (72,8). Used by frame_3ds.cpp and
// Mode1LayerPlan; host-tested by the native canvas, Mode 1 and panorama tests.

namespace starfox::platform_3ds {

// The scene, chosen from the game flow by frame_3ds.cpp; it decides how
// compose_native_canvas fills the added margins.
enum class NativeCanvasStyle {
    gameplay,
    intro,
    controls,
    star_map,
    space,
    game_over,
    plain,
};

// Compose a native top-screen image without scaling the cartridge's 256x224
// artwork. Only world geometry outside that artwork is copied into the added
// gameplay columns; the source frame keeps its original layer priority.
// Needs a 256x224 source and 400x240 world and target, else writes nothing.
// background_left/right are optional 224-entry background-only edge columns;
// intro_vertical_shift moves the intro's margin stars with the horizon.
void compose_native_canvas(const render::Framebuffer& source,
    const render::Framebuffer& world, render::Framebuffer& target,
    const simulation::SnesPpuState& ppu, NativeCanvasStyle style,
    std::span<const std::uint8_t> background_left = {},
    std::span<const std::uint8_t> background_right = {},
    int intro_vertical_shift = 0) noexcept;

// Build once from the unscrolled complete BG3 tilemap. Texture patches are
// anchored in background coordinates; changing HOFS must never choose new
// patches. The padding covers the signed ten-bit SNES scroll range and the
// additional 3DS columns. Both SNES priority planes remain independent.
inline constexpr unsigned native_panorama_padding = 640U;
// The sources must share a power-of-two size from 256 to 1024 pixels per side,
// else nothing is written. Targets become the source width plus twice the
// padding; the map keeps its pixels between the two paddings.
void build_native_panorama_atlas(const render::Framebuffer& source_low,
    const render::Framebuffer& source_high, render::Framebuffer& target_low,
    render::Framebuffer& target_high,
    const simulation::SnesPpuState& ppu);

// Sample the retained atlas for the current scroll. The background uses the
// same coordinates throughout the 400x240 screen, including its centre.
// Applies BG3 scroll and mosaic. Writes nothing unless the targets are 400x240
// and the atlas has the shape built above.
void scroll_native_panorama_atlas(const render::Framebuffer& atlas_low,
    const render::Framebuffer& atlas_high, render::Framebuffer& target_low,
    render::Framebuffer& target_high,
    const simulation::SnesPpuState& ppu) noexcept;

// Compose Mode 1's added pixels once in SNES priority order, then copy the
// already completed source window. The centre needs no second layer pass.
void compose_native_mode1(const render::Framebuffer& source,
    const render::Framebuffer& world, const render::Framebuffer& bg2_low,
    const render::Framebuffer& bg2_high, const render::Framebuffer& bg3_low,
    const render::Framebuffer& bg3_high, render::Framebuffer& target,
    bool bg3_high_priority) noexcept;

// The PICA world is drawn separately. Compose only its three background
// planes, four pixels at a time, without scanning empty world/foreground
// buffers. Decline metadata-bearing targets so the general path owns them.
// Returns false, writing nothing, for those targets or unexpected sizes.
bool compose_native_mode1_background(const render::Framebuffer& source,
    const render::Framebuffer& bg2, const render::Framebuffer& bg3_low,
    const render::Framebuffer& bg3_high, render::Framebuffer& target,
    bool include_bg3_high) noexcept;

// Place only source pixels actually written by foreground passes over the
// native world. Palette index zero and same-colour writes remain opaque when
// their coverage bit is set; unwritten centre pixels retain the 3D world.
void overlay_source_coverage_on_native(const render::Framebuffer& source,
    render::Framebuffer& target) noexcept;

// A run of covered source pixels: its offsets in the 256x224 source and the
// 400x240 target, and its length in pixels.
struct NativeCoverageSpan {
    std::uint32_t source_offset{}, target_offset{}, count{};
};

// Cache source ownership once per PPU change. The fixed 256x224 source can
// contain at most 128 runs per row; storage never depends on session length.
void capture_native_coverage_spans(const render::Framebuffer& source,
    std::vector<NativeCoverageSpan>& spans);
void overlay_source_coverage_on_native(const render::Framebuffer& source,
    render::Framebuffer& target,
    std::span<const NativeCoverageSpan> spans) noexcept;

} // namespace starfox::platform_3ds
