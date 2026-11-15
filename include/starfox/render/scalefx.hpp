#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include "starfox/render/row_workers.hpp"
// ScaleFX 3x reconstruction, a CPU port of Sp00kyFox's shader passes, for the
// SCALEFX 2D filter in pixel_filter.cpp.
namespace starfox::render {
// One float4 per source pixel for each of passes 0-3, reused across calls.
struct ScaleFxScratch {
    std::array<std::vector<std::array<float,4>>,4> passes;
};
// Standard ScaleFX 3x, defaults threshold=.5, AA/corners enabled.
// Packed ARGB output selects source pixels exactly, including their alpha.
// `output` holds width*height*9 pixels; a size mismatch, a zero dimension or
// an image too large throws std::invalid_argument.
void scale_scalefx(std::span<const std::uint32_t> source,
    std::span<std::uint32_t> output,std::uint32_t width,std::uint32_t height,
    ScaleFxScratch&,RowWorkers&);
}
