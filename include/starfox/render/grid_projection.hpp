#pragma once
#include "starfox/simulation/math.hpp"
#include "starfox/timing/fixed_step.hpp"
#include <array>

// Source-exact camera-space lattice and projection of the 15x15 world grid,
// without drawing. Used by DustRenderer and host tests.
namespace starfox::render {
// Compact camera-space lattice, before projection/clipping. Unlike visible
// screen points this retains geometry needed by both stereo eye frusta.
struct GridLattice {
    std::array<std::int16_t,3> origin{},x_step{},z_step{};
};
// The lattice for this camera and Q15 view matrix: its origin and the steps
// between columns (x_step) and rows (z_step), as wrapping 16-bit words.
[[nodiscard]] GridLattice source_grid_lattice(const timing::RenderTransform&,
    const simulation::MatrixQ15&) noexcept;
// Screen position in source pixels, and camera depth, of one grid point.
struct ProjectedGridPoint { std::int16_t x{},y{},depth{}; };
struct ProjectedGrid {
    std::array<ProjectedGridPoint,225> points{};
    std::size_t count{};
};
// Source-order visible lattice points; no pixels, endpoint history or GPU work.
// Points at depth 256 or less, or outside width x height, are omitted. The
// last point gives the endpoint that advances the canonical connected-grid
// history once per source frame (DustRenderer::prepare_grid_lines).
[[nodiscard]] ProjectedGrid project_source_grid(const timing::RenderTransform&,
    const simulation::MatrixQ15&,std::uint32_t width,std::uint32_t height) noexcept;
}
