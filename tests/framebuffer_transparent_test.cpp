#include "starfox/render/framebuffer.hpp"

#include <cstdio>
#include <random>
#include <stdexcept>

using namespace starfox::render;

int main() try {
    std::mt19937 random{0x710a1faU};
    unsigned cases = 0U;
    for (const auto size : {1U, 2U, 3U, 4U, 5U, 15U, 16U, 17U, 257U, 96000U}) {
        Framebuffer source(size, 1U), destination(size, 1U);
        for (unsigned flags = 0U; flags < 8U; ++flags) {
            source.enable_layer_tags((flags & 1U) != 0U);
            destination.enable_layer_tags((flags & 2U) != 0U);
            for (unsigned trial = 0U; trial < 64U; ++trial) {
                for (std::size_t i = 0U; i < size; ++i) {
                    source.pixels()[i] = trial < 4U ? std::array<std::uint8_t,4U>{0U,1U,128U,255U}[trial]
                        : (random() % 3U == 0U ? 0U : static_cast<std::uint8_t>(random()));
                    if (source.layer_tags_enabled()) source.layer_tags()[i] = random() % 5U;
                }
                destination.assign_transparent_layer(source, (flags & 4U) != 0U);
                if (source.pixels() != destination.pixels()) throw std::runtime_error("pixel copy");
                if (destination.tracks_write_coverage() != ((flags & 4U) != 0U)) throw std::runtime_error("tracking");
                for (std::size_t i = 0U; i < size; ++i) {
                    if ((flags & 4U) && destination.write_coverage()[i] != (source.pixels()[i] != 0U))
                        throw std::runtime_error("transparent coverage");
                    const auto tag = source.pixels()[i] != 0U && source.layer_tags_enabled()
                        ? source.layer_tags()[i] : 0U;
                    if (destination.layer_tags_enabled() && destination.layer_tags()[i] != tag)
                        throw std::runtime_error("transparent tag");
                }
                ++cases;
            }
        }
    }
    std::printf("%u exact transparent assignments (pixels/coverage/tags; scalar tails; all flag combinations)\n", cases);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
