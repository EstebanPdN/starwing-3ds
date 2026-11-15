#pragma once
#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif
// Signed 16-bit saturation for audio samples: clamps to [-32768, 32767], with
// ARM SSAT when available. Used by brr_decode.hpp and, through
// tools/cmake/SpcDspOptimizations.cmake, as the pinned SPC_DSP core's CLAMP16.
namespace starfox::audio {
inline int saturate16(int value) noexcept {
#if defined(__ARM_FEATURE_SAT)
    return __ssat(value,16);
#else
    return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
#endif
}
} // namespace starfox::audio
