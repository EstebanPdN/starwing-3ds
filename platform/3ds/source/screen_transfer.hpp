#pragma once
#include <cstddef>
#include <cstdint>

namespace starfox::platform_3ds {
// Native framebuffer columns are contiguous, with Y reversed. Tile the source
// reads and write consecutive destination words instead of scattering every
// store by 960 bytes into the GPU-visible framebuffer.
inline void transfer_rgba_screen(const std::uint32_t* source,
    std::uint32_t* destination, int width, unsigned brightness) {
    for (int by = 0; by < 240; by += 8) {
        for (int bx = 0; bx < width; bx += 8) {
            for (int x = bx; x < bx + 8 && x < width; ++x) {
                auto* out = destination + std::size_t(x) * 240U + 232U - by;
                for (int n = 0; n < 8; ++n) {
                    auto c = source[std::size_t(by + 7 - n) * width + x];
                    if (brightness != 15U) {
                        c = (((c >> 24U) * brightness / 15U) << 24U)
                            | ((((c >> 16U) & 255U) * brightness / 15U) << 16U)
                            | ((((c >> 8U) & 255U) * brightness / 15U) << 8U) | 255U;
                    }
                    out[n] = c;
                }
            }
        }
    }
}
}
