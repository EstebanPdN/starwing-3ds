#pragma once
#include "starfox/render/raster_commands.hpp"
#include <cstddef>
#include <optional>

namespace starfox::platform_3ds::native_engine {
// A band of rows drawn in one colour pair. Its sides are vertical unless
// `slanted`: then the corners below place them, and left/right only bound it.
struct Rectangle {
    std::int16_t left{}, top{}, right{}, bottom{};
    std::uint8_t even{}, odd{};
    std::uint16_t scale{1};
    // Sixteenths of a pixel, the PICA rasterizer's grid. A pixel is drawn
    // when its centre lies inside (left and top sides included), so a side
    // that must cover every pixel it touches stands half a pixel outside.
    std::int16_t top_left{}, top_right{}, bottom_left{}, bottom_right{};
    std::int16_t top_y{}, bottom_y{};
    bool slanted{};
};
struct SceneStats {
    std::size_t source_commands{}, rectangles{}, sampled_pixels{}, covered_pixels{};
    bool overflow{};
};
// Painter order and palette-zero coverage are part of the contract.
class Scene {
public:
    static constexpr std::size_t capacity = 16384;
    Scene() { rectangles_.reserve(capacity); }
    bool compile(const render::RasterCommands& source);
    const std::vector<Rectangle>& rectangles() const noexcept { return rectangles_; }
    const SceneStats& stats() const noexcept { return stats_; }
    static std::optional<std::uint8_t> sample(const render::RasterCommands&,
        const render::RasterCommand&, int x, int y) noexcept;
private:
    // The numbers of an edge run, from a whole command or a packed record.
    struct EdgeRun { std::int32_t left, top, right, bottom, u, v, du, dv; };
    bool append(Rectangle);
    bool append_edge_run(const EdgeRun&, std::uint8_t even,
        std::uint8_t odd, std::uint16_t scale, int width, int height);
    bool compile_edge_run(const EdgeRun&, std::uint8_t even,
        std::uint8_t odd, std::uint16_t scale, int width, int height);
    bool compile_command(const render::RasterCommands&, const render::RasterCommand&,
        int width, int height, std::uint32_t uniform_scale);
    // True when the command was handled; `ok` is then append()'s verdict.
    bool append_texture_spans(const render::RasterCommands&,
        const render::RasterCommand&, int left, int top, int right, int bottom, bool& ok);
    std::vector<Rectangle> rectangles_;
    SceneStats stats_{};
};
} // namespace starfox::platform_3ds::native_engine
