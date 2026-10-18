#pragma once

#include "starfox/render/framebuffer.hpp"
#include "starfox/simulation/snes_ppu.hpp"

#include <span>

namespace starfox::platform_3ds {

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
void build_native_panorama_atlas(const render::Framebuffer& source_low,
    const render::Framebuffer& source_high, render::Framebuffer& target_low,
    render::Framebuffer& target_high,
    const simulation::SnesPpuState& ppu);

// Sample the retained atlas for the current scroll. The background uses the
// same coordinates throughout the 400x240 screen, including its centre.
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

// Place only source pixels actually written by foreground passes over the
// native world. Palette index zero and same-colour writes remain opaque when
// their coverage bit is set; unwritten centre pixels retain the 3D world.
void overlay_source_coverage_on_native(const render::Framebuffer& source,
    render::Framebuffer& target) noexcept;

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
