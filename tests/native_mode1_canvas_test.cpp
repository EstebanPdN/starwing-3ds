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
    // The separate GPU world leaves only BG3 low/high and BG2 low below it.
    // Exercise all byte values and transparent lanes against the full path.
    actual.end_write_coverage();
    expected.end_write_coverage();
    actual.enable_layer_tags(false);
    expected.enable_layer_tags(false);
    world.clear(); bg2_high.clear();
    render::Framebuffer empty{400, 240};
    for (unsigned trial = 0U; trial < 128U; ++trial) {
        for (auto* layer : {&source, &bg2_low, &bg3_low, &bg3_high})
            for (auto& pixel : layer->pixels())
                pixel = (random() & 3U) ? 0U : random() & 255U;
        const bool include_high = trial & 1U;
        platform_3ds::compose_native_mode1(source, world, bg2_low, bg2_high,
            bg3_low, include_high ? bg3_high : empty, expected, false);
        if (!platform_3ds::compose_native_mode1_background(source, bg2_low,
                bg3_low, bg3_high, actual, include_high)
            || actual.pixels() != expected.pixels())
            throw std::runtime_error("GPU background planes differ");
        ++cases;
    }
    actual.begin_write_coverage();
    if (platform_3ds::compose_native_mode1_background(source, bg2_low,
            bg3_low, bg3_high, actual, true))
        throw std::runtime_error("Coverage target bypassed the general compositor");

    std::vector<platform_3ds::NativeCoverageSpan> spans, reference;
    for (unsigned trial = 0U; trial < 64U; ++trial) {
        source.begin_write_coverage();
        for (unsigned y = 0U; y < 224U; ++y)
            for (unsigned x = 0U; x < 256U; ++x) {
                const bool written = trial == 0U ? false : trial == 1U ? true
                    : trial == 2U ? bool(x & 1U) : trial == 3U ? x == 255U
                    : (random() % (trial + 1U)) == 0U;
                if (written) source.set(x, y, 0U);
            }
        source.end_write_coverage();
        reference.clear();
        const auto coverage = source.write_coverage();
        for (unsigned y = 0U; y < 224U; ++y) {
            for (unsigned x = 0U; x < 256U;) {
                while (x < 256U && !coverage[y * 256U + x]) ++x;
                const auto first = x;
                while (x < 256U && coverage[y * 256U + x]) ++x;
                if (x != first) reference.push_back({y * 256U + first,
                    (y + 8U) * 400U + 72U + first, x - first});
            }
        }
        platform_3ds::capture_native_coverage_spans(source, spans);
        const auto equal = [](const auto& a, const auto& b) {
            return a.source_offset == b.source_offset
                && a.target_offset == b.target_offset && a.count == b.count;
        };
        if (!std::equal(spans.begin(), spans.end(), reference.begin(), reference.end(), equal))
            throw std::runtime_error("Coverage runs differ from scalar traversal");
    }
    std::cout << "PASS " << cases
        << " native Mode 1 composites: 400x240 pixels, both BG3 priorities,"
           " transparent and opaque source pixels, tags and 64 coverage patterns\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
