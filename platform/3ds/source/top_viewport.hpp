#pragma once
#include <algorithm>
#include <cstdint>
// Where a picture lands on the 400x240 top screen in wide and 4:3 modes. Used
// by display_3ds.cpp, gpu_self_test.cpp and screen_transfer.hpp; host-tested
// by tests/top_viewport_test.cpp.

namespace starfox::platform_3ds {
// The source rectangle (source_*) and the screen rectangle it fills, in pixels.
struct TopViewport {
    unsigned source_x, source_y, source_width, source_height;
    unsigned x, y, width, height;
    // Match nearest texture sampling at destination pixel centres.
    constexpr unsigned sample_x(unsigned column) const {
        return source_x + ((column - x) * 2U + 1U) * source_width / (width * 2U);
    }
    constexpr unsigned sample_y(unsigned row) const {
        return source_y + ((row - y) * 2U + 1U) * source_height / (height * 2U);
    }
};

// For a width x height picture (clamped to 400x240). In 4:3 mode the centred
// 256x224 SNES area of a large enough picture scales to 320x240 at x = 40;
// otherwise the picture is centred unscaled.
constexpr TopViewport top_viewport(unsigned width, unsigned height, bool wide) {
    width = std::min(width, 400U);
    height = std::min(height, 240U);
    if (!wide && width >= 256U && height >= 224U)
        return {(width-256U)/2U, (height-224U)/2U, 256U, 224U, 40U, 0U, 320U, 240U};
    return {0U, 0U, width, height, (400U-width)/2U, (240U-height)/2U, width, height};
}
} // namespace starfox::platform_3ds
