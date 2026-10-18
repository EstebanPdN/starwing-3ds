#pragma once

#include "starfox/simulation/math.hpp"

namespace starfox::platform_3ds {

inline simulation::MatrixQ15 compose_model_view_matrix(
    const simulation::MatrixQ15& object,
    const simulation::MatrixQ15& view, double interpolation_alpha) noexcept {
    // Quaternion interpolation rounds normalized coefficients back to Q15.
    // Their dot product can reach +32768: source arithmetic wraps that to
    // -32768 and mirrors an entire model for one presentation. Saturate only
    // the generated in-between poses; completed source endpoints retain the
    // cartridge's exact arithmetic.
    return interpolation_alpha > 0.0 && interpolation_alpha < 1.0
        ? simulation::multiply_presentation_matrix_q15(object, view)
        : simulation::multiply_matrix_q15(object, view);
}

} // namespace starfox::platform_3ds
