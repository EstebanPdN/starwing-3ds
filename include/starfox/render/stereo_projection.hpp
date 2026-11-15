#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Inward stereo disparity shared by the software projection paths (models,
// dust, particles, projected text); host tests check it.
namespace starfox::render {

// Parallel eye cameras with the nearest 256 source units on the screen.
// Clamp nearer geometry to that plane: every visible disparity opens into
// the display, and no projectile or clipped polygon can pop out of it.
// Returns the screen shift in source pixels for eye offset `eye` at camera
// depth `depth`: none up to 256, approaching `eye` with distance.
inline std::int32_t inward_stereo_shift(std::int32_t eye, double depth) noexcept {
    if (eye == 0 || depth <= 256.0) return 0;
    return static_cast<std::int32_t>(std::lround(
        eye * std::clamp(1.0 - 256.0 / depth, 0.0, 1.0)));
}

} // namespace starfox::render
