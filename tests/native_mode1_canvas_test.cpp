#include "../platform/3ds/source/native_canvas.hpp"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace starfox;

int main() try {
    std::mt19937 random(0x782403d5U);
    render::Framebuffer source{256, 224}, world{400, 240};
    render::Framebuffer bg2_low{400, 240}, bg2_high{400, 240};
    render::Framebuffer bg3_low{400, 240}, bg3_high{400, 240};
    render::Framebuffer actual{400, 240}, expected{400, 240};
    unsigned cases{};
    for (unsigned trial = 0U; trial < 96U; ++trial) {
        unsigned layer{};
        for (auto* frame : {&source, &world, &bg2_low, &bg2_high,
                &bg3_low, &bg3_high}) {
            for (auto& pixel : frame->pixels())
                pixel = trial < 6U ? (layer == trial ? 255U : 0U)
                    : random() % 3U == 0U ? 0U : random() & 255U;
            ++layer;
        }
        const bool priority = (trial & 1U) != 0U;
        const bool tags = (trial & 2U) != 0U;
        const bool coverage = (trial & 4U) != 0U;
        actual.enable_layer_tags(tags);
        expected.enable_layer_tags(tags);
        actual.end_write_coverage();
        expected.end_write_coverage();
        actual.clear(127U);
        expected.clear(127U);
        if (coverage) {
            actual.begin_write_coverage();
            expected.begin_write_coverage();
        }
        expected.clear();
        render::composite_transparent_layer(bg3_low, expected, {});
        if (!priority) render::composite_transparent_layer(bg3_high, expected, {});
        render::composite_transparent_layer(bg2_low, expected, {});
        render::composite_transparent_layer(world, expected, {});
        render::composite_transparent_layer(bg2_high, expected, {});
        if (priority) render::composite_transparent_layer(bg3_high, expected, {});
        for (unsigned y = 0U; y < 224U; ++y)
            std::copy_n(source.pixels().data() + y * 256U, 256U,
                expected.pixels().data() + (y + 8U) * 400U + 72U);
        platform_3ds::compose_native_mode1(source, world, bg2_low, bg2_high,
            bg3_low, bg3_high, actual, priority);
        if (actual.pixels() != expected.pixels()
            || actual.layer_tags() != expected.layer_tags()
            || !std::equal(actual.write_coverage().begin(),
                actual.write_coverage().end(), expected.write_coverage().begin(),
                expected.write_coverage().end()))
            throw std::runtime_error("Native Mode 1 priority, source or ownership differs");
        ++cases;
    }
    std::cout << "PASS " << cases
        << " native Mode 1 composites: 400x240 pixels, both BG3 priorities,"
           " transparent and opaque source pixels, tags and coverage\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
