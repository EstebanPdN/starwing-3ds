#include "../platform/3ds/source/texel_expand.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

using starfox::platform_3ds::Colours16;
using starfox::render::Framebuffer;

namespace {
std::uint32_t next(std::uint32_t& state) {
    state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
    return state;
}
// The per-pixel loops these expanders replace in display_3ds.cpp.
std::uint16_t reference(const Framebuffer& frame, const Colours16& colours,
    bool foreground, std::size_t index) {
    const auto coverage = frame.write_coverage();
    const bool tracked = coverage.size() == frame.pixels().size();
    const auto pixel = frame.pixels()[index];
    if (!foreground) return colours[pixel];
    const bool opaque = tracked ? coverage[index] != 0U : pixel != 0U;
    return opaque ? colours[pixel] : 0U;
}
}

int main() {
    std::uint32_t state = 0x1234567U;
    Colours16 colours{};
    for (auto& colour : colours) colour = static_cast<std::uint16_t>(next(state) | 1U);
    unsigned checked = 0U;
    for (const unsigned width : {400U, 256U, 274U, 3U, 7U}) {
        for (int pattern = 0; pattern < 5; ++pattern) {
            for (const bool track : {false, true}) {
                Framebuffer frame(width, 9U);
                if (track) frame.begin_write_coverage();
                auto& pixels = frame.pixels();
                std::vector<std::uint8_t> coverage(pixels.size());
                for (std::size_t i = 0; i < pixels.size(); ++i) {
                    const auto r = next(state);
                    // Empty runs, sparse sprites, dense fills and index zero
                    // painted opaque all occur in gameplay foregrounds.
                    const bool covered = pattern == 0 ? false : pattern == 1 ? (r & 31U) == 0U
                        : pattern == 2 ? true : pattern == 3 ? (i / 6U) % 2U == 0U : (r & 1U) != 0U;
                    pixels[i] = covered ? static_cast<std::uint8_t>(r >> 8U) : 0U;
                    if (pattern == 3 && covered && (r & 3U) == 0U) pixels[i] = 0U;
                    coverage[i] = covered;
                }
                if (track) {
                    // Framebuffer exposes coverage read-only; mark it through
                    // its writer so tracked frames include opaque index 0.
                    for (std::size_t i = 0; i < pixels.size(); ++i)
                        if (coverage[i]) frame.mark_written(i, 1U);
                }
                for (const bool foreground : {false, true}) {
                    std::vector<std::uint16_t> out(512U * 9U, 0xdeadU);
                    starfox::platform_3ds::expand_frame(frame, colours, foreground, out.data(), 512U);
                    for (unsigned y = 0; y < 9U; ++y)
                        for (unsigned x = 0; x < width; ++x) {
                            assert(out[y * 512U + x] == reference(frame, colours, foreground, y * width + x));
                            ++checked;
                        }
                    for (unsigned y = 0; y < 9U; ++y)
                        for (unsigned x = width; x < 512U; ++x) assert(out[y * 512U + x] == 0xdeadU);
                }
            }
        }
    }
    std::printf("texel expansion: %u texels match the per-pixel reference\n", checked);
}
