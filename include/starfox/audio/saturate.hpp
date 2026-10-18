#pragma once
#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif
namespace starfox::audio {
inline int saturate16(int value) noexcept {
#if defined(__ARM_FEATURE_SAT)
    return __ssat(value,16);
#else
    return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
#endif
}
} // namespace starfox::audio
