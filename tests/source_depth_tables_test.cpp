#include "starfox/render/source_depth_tables.hpp"
#include <cstdio>
#include <random>
#include <stdexcept>

int main() try {
    using namespace starfox;
    std::mt19937 random(0x3d5de97U);
    std::vector<std::uint8_t> bytes(4U * 1024U * 1024U);
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    const assets::RomImage rom(std::move(bytes));
    unsigned cases{};
    for (const auto address : {0x038000U, 0x03fc00U, 0x7e9000U}) {
        for (const auto thresholds : {0x7fffU, 0x8123U, 0xfffdU, 0xffffU}) {
            for (const auto colours : {0x7fffU, 0x8234U, 0xff80U, 0xffffU}) {
                render::SourceDepthTables cached(rom, address, thresholds, colours);
                // Repeat entries after other offsets to exercise retained
                // colours, object overrides, signed thresholds and rejection.
                for (unsigned iteration = 0; iteration < 512U; ++iteration) {
                    const auto offset = static_cast<std::uint8_t>(iteration);
                    render::RenderPose expected, actual;
                    expected.has_depth_colour_tables = true;
                    for (auto& threshold : expected.depth_thresholds) threshold = random();
                    for (auto& table : expected.depth_colour_tables)
                        for (auto& colour : table) colour = static_cast<std::uint8_t>(random());
                    actual = expected;
                    bool reference_threw{}, candidate_threw{};
                    try { render::apply_source_depth_tables(rom, address, thresholds, colours, offset, expected); }
                    catch (const std::exception&) { reference_threw = true; }
                    try { cached.apply(offset, actual); }
                    catch (const std::exception&) { candidate_threw = true; }
                    if (reference_threw != candidate_threw
                        || actual.has_depth_colour_tables != expected.has_depth_colour_tables
                        || actual.depth_thresholds != expected.depth_thresholds
                        || actual.depth_colour_tables != expected.depth_colour_tables)
                        throw std::runtime_error("Cached depth tables differ from source");
                    ++cases;
                }
            }
        }
    }
    std::printf("PASS %u depth-table comparisons: all offsets, reuse, signed bytes, invalid pointers and wrap\n", cases);
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
