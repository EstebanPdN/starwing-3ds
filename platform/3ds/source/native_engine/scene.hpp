#pragma once
#include "starfox/render/raster_commands.hpp"
#include <cstddef>
#include <optional>

namespace starfox::platform_3ds::native_engine {
struct Rectangle {
    std::int16_t left{}, top{}, right{}, bottom{};
    std::uint8_t even{}, odd{};
    std::uint16_t scale{1};
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
    bool append(Rectangle);
    std::vector<Rectangle> rectangles_;
    SceneStats stats_{};
};
} // namespace starfox::platform_3ds::native_engine
