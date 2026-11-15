#pragma once

#include "starfox/render/framebuffer.hpp"
#include "starfox/render/palette.hpp"

#include "starfox/render/row_workers.hpp"
#include "starfox/render/scalefx.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Presentation filters for cartridge 2D art at RENDER UPSCALE. Used by the
// simulation's 2D filter option (backend availability) and host tests.
namespace starfox::render {

// Presentation-time upscaling for cartridge-authored 2D art.
//
// RENDER UPSCALE rasterizes the Super FX world layer at the selected scale,
// but every 2D pass keeps addressing the source raster, so sprites,
// backgrounds, HUD and text arrive as SxS nearest-neighbour blocks. These
// filters replace that block expansion with an edge-directed reconstruction,
// using layer tags to include textured polygon artwork without filtering
// solid-coloured geometry or changing the polygon coverage mask.
enum class TwoDFilter : std::uint8_t {
    off = 0,
    // Edge-directed corner reconstruction. Original implementation, generalized
    // from the classic EPX/Scale2x corner rules to any integer factor, so it
    // tracks RENDER UPSCALE from 2x to 10x. Carries no third-party licence.
    edge = 1,
    // xBRZ by Zenju (GPLv3). This source does not contain the backend:
    // two_d_filter_compiled_in() reports it absent, and apply_two_d_filter()
    // and filter_overlay_layer() fall back to EDGE.
    xbrz = 2,
    // Bilinear with a narrowed transition that keeps pixel centres; CRT adds
    // a bright-phosphor glow and a scanline mask to it.
    sharp_bilinear = 3,
    crt = 4,
    // ScaleFX at a fixed 3x (scalefx.hpp).
    scalefx = 5,
};

inline constexpr std::size_t two_d_filter_count = 6U;

// Upper-case display name; "OFF" for off and unknown values.
[[nodiscard]] std::string_view two_d_filter_name(TwoDFilter filter) noexcept;
// False when the backend was not compiled in; the caller may still select it,
// and apply_two_d_filter() degrades to EDGE.
[[nodiscard]] bool two_d_filter_compiled_in(TwoDFilter filter) noexcept;

// Reusable per-presentation buffers. One instance lives for the lifetime of
// the window so no filtered frame allocates. Threads live in RowWorkers, which
// the presentation passes share.
struct PixelFilterScratch {
    ScaleFxScratch scalefx;
    std::vector<std::uint32_t> source;   // source raster, packed ARGB
    std::vector<std::uint32_t> filtered; // filter output, packed ARGB
};

// Replaces artwork-owned pixels of `rgba` (stored resolution, RGBA8, as
// produced by expand_rgba) with a filtered reconstruction of the same art.
//
// Requires framebuffer.layer_tags_enabled(), a draw scale from 1 to 10 and an
// `rgba` covering the stored frame; otherwise it returns without touching
// `rgba`. Scales above the backend's maximum factor are filtered at that
// maximum and point-sampled up, which still beats block expansion.
// `highlight` paints every pixel the filter claims in magenta instead of the
// filtered colour. Nothing else changes, so anything still showing its own
// art is a pixel the filter is not touching -- the direct way to see which
// layer the runtime believes each part of a real frame belongs to.
void apply_two_d_filter(
    TwoDFilter filter,
    const Framebuffer& framebuffer,
    std::span<const Rgba8> palette,
    std::vector<std::uint8_t>& rgba,
    PixelFilterScratch& scratch,
    RowWorkers& workers,
    bool highlight = false);

// Filters a standalone source-raster layer that composites in RGBA after the
// main frame and so never reaches the tagged framebuffer -- the planet
// sequence's portrait and text overlays, which are entirely cartridge art.
// Fills `out_argb` with stored-resolution ARGB, transparent where the overlay
// has no art, and returns true. Returns false when the filter does not apply
// (off, a render scale outside 1-10, an empty palette, or an overlay with no
// art), leaving the caller's own nearest-neighbour path in charge.
[[nodiscard]] bool filter_overlay_layer(
    TwoDFilter filter,
    const Framebuffer& overlay,
    std::span<const Rgba8> palette,
    std::uint32_t render_scale,
    std::vector<std::uint32_t>& out_argb,
    PixelFilterScratch& scratch,
    RowWorkers& workers);

// Exposed for tests: scales `source` (ARGB, `width` x `height`) into `target`
// by `factor`, treating fully transparent source pixels as holes. Only source
// rows [first_row, last_row) are written; factors outside 2-10 write nothing.
void scale_edge(
    std::size_t factor,
    const std::uint32_t* source,
    std::uint32_t* target,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t first_row,
    std::uint32_t last_row) noexcept;

} // namespace starfox::render
