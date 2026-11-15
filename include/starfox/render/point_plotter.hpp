#pragma once

#include "starfox/render/framebuffer.hpp"

#include <cstdint>

namespace starfox::render {

// Framebuffer::set for a renderer that plots many single points in one pass
// (dust, grid dots, particles). set() asks the raster for its extent, which
// divides by the draw scale, twice per point, and a recording raster then
// builds a 96-byte command and copies it into the stream. The extent, scale,
// layer tag and recorder cannot change during such a pass, so they are read
// once here; a recorded point goes to the recorder as a plain rectangle.
//
// Construct it after any ScopedLayer of the pass: the tag is read once. The
// result is exactly what target.set(x, y, colour) records or draws.
class PointPlotter {
public:
    explicit PointPlotter(Framebuffer& target) noexcept
        : target_(target), commands_(target.command_buffer()),
          width_(static_cast<std::int32_t>(target.width())),
          height_(static_cast<std::int32_t>(target.height())),
          scale_(static_cast<std::int32_t>(target.draw_scale())),
          tag_(target.layer_override() < 0
              ? static_cast<std::uint32_t>(PixelLayer::two_d)
              : static_cast<std::uint32_t>(target.layer_override())) {}

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }

    void set(std::int32_t x, std::int32_t y, std::uint8_t colour) const noexcept {
        if (commands_ == nullptr) {
            target_.set(x, y, colour);
            return;
        }
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        const auto left = x * scale_;
        const auto top = y * scale_;
        commands_->add_solid(left, top, left + scale_, top + scale_, colour, colour, 0U, tag_);
    }

private:
    Framebuffer& target_;
    RasterCommands* commands_;
    std::int32_t width_, height_, scale_;
    std::uint32_t tag_;
};

} // namespace starfox::render
