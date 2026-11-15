#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/render/grid_line_history.hpp"
#include "starfox/render/grid_projection.hpp"
#include "starfox/simulation/dust_system.hpp"
#include "starfox/timing/fixed_step.hpp"

// Star dust and the 15x15 world grid (dots, or MSHOWGRID2's connected lines)
// projected from the source camera, with captured frames for deferred stereo
// eye draws. Used by the 3DS frame code, host tools and tests.
namespace starfox::render {

class DustRenderer {
public:
    // One dust pass captured for later drawing. The offset and exclusion
    // fields mean the same as draw()'s; zero excludes nothing.
    struct DustFrame {
        std::vector<simulation::DustPoint> points;
        std::array<std::uint8_t,64> colours{};
        timing::RenderTransform camera{};
        simulation::MatrixQ15 matrix{};
        std::int32_t offset_x{},offset_y{},exclude_left{},exclude_right{};
    };
    // Own the source points/palette so deferred eye rendering cannot observe
    // the simulation recycling points between recording and submission.
    [[nodiscard]] DustFrame prepare_dust(const simulation::DustSystem&,
        std::size_t active_count,const timing::RenderTransform&,
        const simulation::MatrixQ15&) const;
    // Plots a captured pass like draw(), without stereo disparity.
    static void draw_dust_frame(const DustFrame&,Framebuffer&) noexcept;
    // Binary64 XYZ and padding, matching the portable GPU input record.
    // Only source wrapping happens here; matrix/projection stay on the GPU.
    // Throws std::invalid_argument for more than kMaximumDustPoints points or
    // a non-finite camera.
    [[nodiscard]] static std::vector<std::array<double,4>> pack_dust_points(const DustFrame&);
    // A source frame's visible grid points and the line start carried from
    // the previous source frame (see GridLineHistory).
    struct GridLinesFrame {
        ProjectedGrid projected;
        std::array<std::int16_t,2> start{};
    };
    // Capture once in the source camera, then share with both eye draws.
    // Does not draw pixels; repeated presentations retain the same start.
    // Advances this renderer's grid-line history although it is const.
    [[nodiscard]] GridLinesFrame prepare_grid_lines(
        const timing::RenderTransform& camera,const simulation::MatrixQ15& matrix,
        std::uint64_t source_frame,std::uint32_t width,std::uint32_t height) const noexcept;
    // Draws MSHOWGRID2's markers and connecting lines in `colour` (default:
    // palette 7, entry 14).
    static void draw_grid_lines_frame(const GridLinesFrame&,Framebuffer&,
        std::uint8_t colour=126) noexcept;
    // Reads STAR_COLS from `rom`, which must outlive the renderer. Throws
    // std::runtime_error when the symbol is missing.
    DustRenderer(
        const assets::RomImage& rom,
        const assets::SymbolMap& symbols);

    // Projects the first active_count dust points about the raster centre
    // (moved by projection_offset_x/y) and plots each in its depth-shaded
    // STAR_COLS colour, with a second pixel nearer than depth 1024. Columns
    // in [exclude_left, exclude_right) are skipped; eye_offset adds inward
    // stereo disparity. Writes are tagged world_geometry.
    void draw(
        const simulation::DustSystem& dust,
        std::size_t active_count,
        const timing::RenderTransform& camera,
        const simulation::MatrixQ15& view_matrix,
        Framebuffer& target,
        std::int32_t projection_offset_x = 0,
        std::int32_t projection_offset_y = 0,
        std::int32_t exclude_left = 0,
        std::int32_t exclude_right = 0,
        std::int32_t eye_offset = 0) const noexcept;

    // Plots the 15x15 world grid as dots in palette 7, entry 14, with a
    // second pixel nearer than depth 512; eye_offset and projection_offset_x
    // as for draw().
    void draw_grid(
        const timing::RenderTransform& camera,
        const simulation::MatrixQ15& view_matrix,
        Framebuffer& target, std::int32_t eye_offset = 0,
        std::int32_t projection_offset_x = 0) const noexcept;

    // prepare_grid_lines() for this target's extent, then
    // draw_grid_lines_frame().
    void draw_grid_lines(
        const timing::RenderTransform& camera,
        const simulation::MatrixQ15& view_matrix,
        std::uint64_t source_frame,
        Framebuffer& target) const noexcept;

private:
    const assets::RomImage* rom_{};
    std::uint32_t star_colours_{};
    mutable GridLineHistory grid_line_history_;
};

} // namespace starfox::render
