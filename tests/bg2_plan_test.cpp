#include "../platform/3ds/source/bg2_plan.hpp"
#include "starfox/render/background_renderer.hpp"
#include "starfox/render/framebuffer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

std::uint32_t random_state = 0x725f1964U;
std::size_t widest_plan = 0U;
std::uint32_t random_word() {
    random_state ^= random_state << 13U;
    random_state ^= random_state >> 17U;
    random_state ^= random_state << 5U;
    return random_state;
}

std::uint8_t character_pixel(const starfox::simulation::SnesPpuState& ppu,
    std::uint16_t character, unsigned x, unsigned y) {
    const auto base = (static_cast<std::uint32_t>(ppu.bg2_character_base) * 2U
        + static_cast<std::uint32_t>(character) * 32U + y * 2U) & 0xffffU;
    const auto mask = static_cast<std::uint8_t>(0x80U >> x);
    return static_cast<std::uint8_t>(
        ((ppu.vram[base] & mask) != 0U ? 1U : 0U)
        | ((ppu.vram[(base + 1U) & 0xffffU] & mask) != 0U ? 2U : 0U)
        | ((ppu.vram[(base + 16U) & 0xffffU] & mask) != 0U ? 4U : 0U)
        | ((ppu.vram[(base + 17U) & 0xffffU] & mask) != 0U ? 8U : 0U));
}

[[noreturn]] void fail(const char* why, unsigned case_number,
    unsigned x = 0U, unsigned y = 0U) {
    std::fprintf(stderr, "BG2 plan case %u failed: %s at %u,%u\n",
        case_number, why, x, y);
    std::exit(1);
}

void compare_case(unsigned case_number, unsigned map_size,
    bool tile16, bool vertical, unsigned row_mode,
    unsigned base_phase, bool wide = false) {
    starfox::simulation::SnesPpuState ppu;
    ppu.background_mode = 2U;
    ppu.main_screen = 0x02U;
    ppu.bg2_screen_size = static_cast<std::uint8_t>(map_size);
    ppu.bg2_tile_size_16 = tile16;
    ppu.bg2_character_base = 0x1000U;
    ppu.bg2_screen_base = 0x6000U;
    ppu.bg2_vertical_offsets_enabled = vertical;
    ppu.bg2_horizontal_offsets_enabled = (row_mode & 1U) != 0U;
    ppu.bg2_scanline_scroll_enabled = (row_mode & 2U) != 0U;
    for (auto& byte : ppu.vram)
        byte = static_cast<std::uint8_t>(random_word());
    for (unsigned i = 0U; i < 4096U; ++i) {
        const auto word = static_cast<std::uint16_t>(
            (random_word() & 0x03ffU)
            | ((random_word() & 7U) << 10U)
            | ((random_word() & 3U) << 14U));
        const auto byte = (ppu.bg2_screen_base + i) * 2U;
        ppu.vram[byte] = static_cast<std::uint8_t>(word);
        ppu.vram[byte + 1U] = static_cast<std::uint8_t>(word >> 8U);
    }
    for (unsigned i = 0U; i < 32U; ++i) {
        const auto word = static_cast<std::uint16_t>(
            (random_word() & 0x1fffU)
            | ((random_word() & 1U) != 0U ? 0x4000U : 0U));
        ppu.vram[(0x2fa0U + i) * 2U] = static_cast<std::uint8_t>(word);
        ppu.vram[(0x2fa0U + i) * 2U + 1U] =
            static_cast<std::uint8_t>(word >> 8U);
    }
    for (unsigned y = 0U; y < 224U; ++y) {
        ppu.bg2_horizontal_offsets[y] = static_cast<std::int16_t>(
            static_cast<int>(random_word() & 1023U) - 512);
        ppu.bg2_scanline_scroll_y[y] = static_cast<std::int16_t>(
            static_cast<int>(random_word() & 1023U) - 512);
    }
    const auto scroll_x = static_cast<std::int32_t>(
        static_cast<int>(random_word() & 1023U) - 512) * 8 +
        static_cast<int>(base_phase);
    const auto scroll_y = static_cast<std::int32_t>(
        static_cast<int>(random_word() & 1023U) - 512);

    std::vector<starfox::platform_3ds::Bg2Rect> rects;
    const auto target_width = wide ? 400U : 256U;
    const auto target_height = wide ? 240U : 224U;
    const auto origin_x = wide ? 72 : 0;
    const auto origin_y = wide ? 8 : 0;
    if (!starfox::platform_3ds::plan_bg2_rects(
            ppu, scroll_x, scroll_y, rects, 100000U,
            target_width, target_height, origin_x, origin_y)) {
        fail("planner rejected supported state", case_number);
    }
    std::vector<starfox::platform_3ds::Bg2Rect> uncached_rects;
    if (!starfox::platform_3ds::plan_bg2_rects(
            ppu, scroll_x, scroll_y, uncached_rects, 100000U,
            target_width, target_height, origin_x, origin_y, false)
        || rects != uncached_rects) {
        fail("cached plan differs from uncached reference", case_number);
    }
    if (wide) widest_plan = std::max(widest_plan, rects.size());
    if (wide && rects.size() > 16384U) {
        std::vector<starfox::platform_3ds::Bg2Rect> bounded{{}};
        if (starfox::platform_3ds::plan_bg2_rects(ppu,
                scroll_x, scroll_y, bounded, 16384U,
                target_width, target_height, origin_x, origin_y)
            || !bounded.empty())
            fail("wide capacity did not fail atomically", case_number);
    }
    std::vector<std::uint8_t> actual(target_width * target_height, 233U);
    std::vector<std::uint8_t> coverage(actual.size());
    for (const auto& rect : rects) {
        if (rect.width == 0U || rect.height == 0U
            || rect.x + rect.width > target_width
            || rect.y + rect.height > target_height
            || rect.source_x > 7U || rect.source_y > 7U) {
            fail("invalid rectangle", case_number, rect.x, rect.y);
        }
        for (unsigned dy = 0U; dy < rect.height; ++dy) {
            for (unsigned dx = 0U; dx < rect.width; ++dx) {
                const auto x = rect.x + dx;
                const auto y = rect.y + dy;
                const auto index = y * target_width + x;
                if (++coverage[index] != 1U && !rect.solid)
                    fail("overlapping rectangles", case_number, x, y);
                if (rect.solid) {
                    actual[index] = rect.solid_colour;
                    continue;
                }
                const auto source_x = rect.reverse_x
                    ? rect.source_x - dx : rect.source_x + dx;
                const auto source_y = rect.reverse_y
                    ? rect.source_y - dy : rect.source_y + dy;
                if (source_x > 7U || source_y > 7U)
                    fail("rectangle crosses character", case_number, x, y);
                const auto colour = character_pixel(ppu, rect.character,
                    source_x, source_y);
                if (colour != 0U) {
                    actual[index] = static_cast<std::uint8_t>(
                        rect.palette_bank * 16U + colour);
                }
            }
        }
    }
    starfox::render::Framebuffer reference{target_width, target_height};
    reference.clear(233U);
    starfox::render::BackgroundRenderer{}.draw_bg2(ppu,
        scroll_x, scroll_y, reference,
        starfox::render::TilePriorityPass::all,
        origin_x, true, true, false, 0U, {}, origin_y);
    for (unsigned y = 0U; y < target_height; ++y) {
        for (unsigned x = 0U; x < target_width; ++x) {
            const auto index = y * target_width + x;
            if (coverage[index] == 0U)
                fail("missing rectangle", case_number, x, y);
            if (actual[index] != reference.get(x, y)) {
                std::fprintf(stderr, "rect=%u cpu=%u\n",
                    actual[index], reference.get(x, y));
                fail("pixel differs from CPU renderer", case_number, x, y);
            }
        }
    }
}

void rolled_ground_regression() {
    starfox::simulation::SnesPpuState ppu;
    ppu.background_mode = 2U;
    ppu.main_screen = 0x02U;
    ppu.bg2_screen_size = 3U;
    ppu.bg2_screen_base = 0x6000U;
    ppu.bg2_character_base = 0x1000U;
    ppu.bg2_vertical_offsets_enabled = true;
    for (unsigned i = 0U; i < 32U; ++i) {
        const auto address = (0x2fa0U + i) * 2U;
        ppu.vram[address] = 320U & 255U;
        ppu.vram[address + 1U] = 0x40U | (320U >> 8U);
    }
    for (unsigned tile_y = 0U; tile_y < 64U; ++tile_y) {
        for (unsigned tile_x = 0U; tile_x < 64U; ++tile_x) {
            const auto page = (tile_x >> 5U) + (tile_y >> 5U) * 2U;
            const auto entry = page * 0x400U
                + (tile_y & 31U) * 32U + (tile_x & 31U);
            const auto address = (0x6000U + entry) * 2U;
            const auto character = tile_y == 60U ? 3U
                : tile_y >= 45U ? 2U : 1U;
            ppu.vram[address] = static_cast<std::uint8_t>(character);
        }
    }
    for (unsigned y = 0U; y < 8U; ++y) {
        ppu.vram[0x2000U + 1U * 32U + y * 2U] = 0xffU;
        ppu.vram[0x2000U + 2U * 32U + y * 2U + 1U] = 0xffU;
    }
    std::vector<starfox::platform_3ds::Bg2Rect> rects;
    if (!starfox::platform_3ds::plan_bg2_rects(
            ppu, 0, 0, rects, 16383U, 400U, 240U, 72, 8))
        fail("rolled ground exceeded GPU plan capacity", 529U);
    // Cross-column merging collapses the two repeated correction runs from
    // 800 one-column rectangles to two wide rectangles in this fixture.
    if (rects.size() != 1502U)
        fail("rolled correction runs were not merged", 529U);
    std::vector<starfox::platform_3ds::Bg2Rect> uncached_rects;
    if (!starfox::platform_3ds::plan_bg2_rects(
            ppu, 0, 0, uncached_rects, 16383U, 400U, 240U, 72, 8,
            false)
        || rects != uncached_rects)
        fail("rolled cached plan differs from uncached plan", 529U);
    const auto solid_at = [&](unsigned x, unsigned y) {
        std::uint8_t result = 0U;
        for (const auto& rect : rects)
            if (rect.solid && x >= rect.x && x < rect.x + rect.width
                && y >= rect.y && y < rect.y + rect.height)
                result = rect.solid_colour;
        return result;
    };
    if (solid_at(200U, 168U) != 2U
        || solid_at(200U, 220U) != 2U
        || solid_at(200U, 100U) != 0U)
        fail("transparent and wrapped ground did not continue", 529U);

    std::vector<std::uint8_t> actual(400U * 240U, 233U);
    std::vector<std::uint8_t> coverage(actual.size());
    for (const auto& rect : rects) {
        if (rect.width == 0U || rect.height == 0U
            || rect.x + rect.width > 400U || rect.y + rect.height > 240U)
            fail("rolled plan emitted an invalid rectangle", 529U,
                rect.x, rect.y);
        for (unsigned dy = 0U; dy < rect.height; ++dy) {
            for (unsigned dx = 0U; dx < rect.width; ++dx) {
                const auto x = rect.x + dx;
                const auto y = rect.y + dy;
                const auto index = y * 400U + x;
                if (++coverage[index] != 1U && !rect.solid)
                    fail("rolled plan emitted overlapping tiles", 529U, x, y);
                if (rect.solid) {
                    actual[index] = rect.solid_colour;
                    continue;
                }
                const auto source_x = rect.reverse_x
                    ? rect.source_x - dx : rect.source_x + dx;
                const auto source_y = rect.reverse_y
                    ? rect.source_y - dy : rect.source_y + dy;
                const auto colour = character_pixel(ppu, rect.character,
                    source_x, source_y);
                if (colour != 0U) {
                    actual[index] = static_cast<std::uint8_t>(
                        rect.palette_bank * 16U + colour);
                }
            }
        }
    }
    starfox::render::Framebuffer reference{400U, 240U};
    reference.clear(233U);
    starfox::render::BackgroundRenderer{}.draw_bg2(ppu, 0, 0, reference,
        starfox::render::TilePriorityPass::all,
        72, true, true, false, 0U, {}, 8);
    for (unsigned y = 0U; y < 240U; ++y) {
        for (unsigned x = 0U; x < 400U; ++x) {
            const auto index = y * 400U + x;
            if (coverage[index] == 0U)
                fail("rolled plan left a pixel uncovered", 529U, x, y);
            if (actual[index] != reference.get(x, y))
                fail("merged rolled plan differs from CPU renderer", 529U,
                    x, y);
        }
    }

    // Bounded host comparison: same synthetic tilemap/viewport with and
    // without rolled-ground planning. This measures planner CPU cost only.
    const auto measure = [&](bool rolled, bool cache_samples) {
        ppu.bg2_vertical_offsets_enabled = rolled;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned repeat = 0U; repeat < 64U; ++repeat) {
            if (!starfox::platform_3ds::plan_bg2_rects(
                    ppu, 0, 0, rects, 16383U, 400U, 240U, 72, 8,
                    cache_samples))
                fail("benchmark plan exceeded capacity", 529U);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration_cast<std::chrono::microseconds>(
            elapsed).count() / 64.0;
    };
    const auto ordinary_us = measure(false, true);
    const auto rolled_uncached_us = measure(true, false);
    const auto rolled_cached_us = measure(true, true);
    std::printf("BG2 synthetic planner host timing: ordinary %.1f us, "
        "rolled uncached %.1f us, rolled cached %.1f us per plan\n",
        ordinary_us, rolled_uncached_us, rolled_cached_us);
}

void uniform_ground_regression() {
    starfox::simulation::SnesPpuState ppu;
    ppu.main_screen = 0x02U;
    ppu.bg2_screen_base = 0x6000U;
    ppu.bg2_character_base = 0x1000U;
    ppu.bg2_screen_size = 3U;
    ppu.bg2_vertical_offsets_enabled = true;
    for (unsigned column = 0; column < 32; ++column) {
        const auto offset = 0x4000U | (280U + column * 2U);
        ppu.vram[(0x2fa0U + column) * 2U] = offset & 255U;
        ppu.vram[(0x2fa0U + column) * 2U + 1U] = offset >> 8U;
    }
    // All four bitplanes, transparent characters, mixed characters, palette
    // changes and flips must give the identical plan after the shortcut.
    for (unsigned character = 0; character < 64; ++character)
        for (unsigned row = 0; row < 8; ++row)
            for (unsigned plane = 0; plane < 4; ++plane) {
                const auto address = 0x2000U + character * 32U + row * 2U
                    + (plane / 2U) * 16U + plane % 2U;
                ppu.vram[address] = (character & (1U << plane)) ? 255U : 0U;
                if (character >= 32U && row == 7U) ppu.vram[address] ^= 1U;
            }
    for (unsigned entry = 0; entry < 4096; ++entry) {
        const auto word = (entry % 32U) | (((entry / 32U) & 7U) << 10U)
            | (((entry / 256U) & 3U) << 14U);
        ppu.vram[0xc000U + entry * 2U] = word & 255U;
        ppu.vram[0xc000U + entry * 2U + 1U] = word >> 8U;
    }
    for (const bool tile16 : {false, true}) for (unsigned phase = 0; phase < 8; ++phase) {
        ppu.bg2_tile_size_16 = tile16;
        ppu.bg2_horizontal_offsets_enabled = (phase & 1U) != 0;
        for (unsigned y = 0; y < 224; ++y)
            ppu.bg2_horizontal_offsets[y] = static_cast<std::int16_t>(int(y / 8U) - 14);
        std::vector<starfox::platform_3ds::Bg2Rect> cached, reference;
        if (!starfox::platform_3ds::plan_bg2_rects(ppu, int(phase) - 4, 0,
                cached, 100000U, 400U, 240U, 72, 8, true)
            || !starfox::platform_3ds::plan_bg2_rects(ppu, int(phase) - 4, 0,
                reference, 100000U, 400U, 240U, 72, 8, false)
            || cached != reference)
            fail("uniform/transparent/mixed ground shortcut changed the plan", 800U + phase);
    }
    std::printf("BG2 uniform ground: 16 palette/flip/transparency states passed\n");
}

void irrelevant_register_bits_regression() {
    starfox::simulation::SnesPpuState ppu;
    ppu.background_mode = 2U;
    ppu.main_screen = 0x02U;
    ppu.mosaic = 0U;
    std::vector<starfox::platform_3ds::Bg2Rect> reference;
    if (!starfox::platform_3ds::plan_bg2_rects(ppu, 0, 0, reference,
            16383U, 400U, 240U, 72, 8)) {
        fail("reference plan rejected irrelevant-register case", 700U);
    }

    ppu.main_screen = 0xf2U;
    ppu.mosaic = 0xfcU;
    std::vector<starfox::platform_3ds::Bg2Rect> changed;
    if (!starfox::platform_3ds::plan_bg2_rects(ppu, 0, 0, changed,
            16383U, 400U, 240U, 72, 8)
        || changed != reference) {
        fail("unrelated screen/mosaic bits changed the BG2 plan", 700U);
    }
}

} // namespace

int main() {
    unsigned cases = 0U;
    for (unsigned map_size = 0U; map_size < 4U; ++map_size) {
        for (const bool tile16 : {false, true}) {
            for (const bool vertical : {false, true}) {
                for (unsigned row_mode = 0U; row_mode < 4U; ++row_mode) {
                    for (unsigned phase = 0U; phase < 8U; ++phase) {
                        compare_case(++cases, map_size, tile16,
                            vertical, row_mode, phase);
                    }
                }
            }
        }
    }
    rolled_ground_regression();
    uniform_ground_regression();
    irrelevant_register_bits_regression();
    // The expanded raster must preserve every pixel of the original viewport
    // while providing valid, unscaled tile fragments across the extra area.
    for (unsigned map_size = 0U; map_size < 4U; ++map_size) {
        for (unsigned row_mode = 0U; row_mode < 4U; ++row_mode) {
            compare_case(++cases, map_size, (map_size & 1U) != 0U,
                (map_size & 2U) != 0U, row_mode, map_size, true);
        }
    }
    for (unsigned sample = 0U; sample < 32U; ++sample) {
        compare_case(++cases, random_word() & 3U,
            (random_word() & 1U) != 0U, true, random_word() & 3U,
            random_word() & 7U, true);
    }
    starfox::simulation::SnesPpuState ppu;
    std::vector<starfox::platform_3ds::Bg2Rect> rects{{}};
    ppu.mosaic = 0x02U;
    if (starfox::platform_3ds::plan_bg2_rects(ppu, 0, 0, rects)
        || !rects.empty()) fail("mosaic did not fail atomically", cases);
    ppu.mosaic = 0U;
    if (starfox::platform_3ds::plan_bg2_rects(ppu, 0, 0, rects, 1U)
        || !rects.empty()) fail("capacity did not fail atomically", cases);
    std::printf("BG2 planner parity: %u randomized states passed; "
        "maximum wide fragments %zu\n", cases, widest_plan);
}
