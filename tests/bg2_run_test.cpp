// The 256x224 BG2 run renderer must match the per-pixel renderer exactly.
// A BG2 mosaic of size one selects the per-pixel path while sampling every
// pixel at its own coordinate, so the two images are directly comparable.
#include "starfox/render/background_renderer.hpp"

#include <cassert>
#include <cstdio>
#include <memory>

namespace {
std::uint32_t next(std::uint32_t& state) {
    state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
    return state;
}
}

int main() {
    using starfox::render::Framebuffer;
    using starfox::render::TilePriorityPass;
    std::uint32_t state = 0x9e3779b9U;
    starfox::render::BackgroundRenderer renderer;
    unsigned compared = 0U;
    for (unsigned trial = 0U; trial < 400U; ++trial) {
        auto ppu = std::make_unique<starfox::simulation::SnesPpuState>();
        for (auto& byte : ppu->vram) byte = static_cast<std::uint8_t>(next(state));
        // Sparse characters give transparent runs; flips and priorities vary.
        if (trial % 3U == 0U)
            for (std::size_t i = 0; i < ppu->vram.size(); ++i)
                if ((next(state) & 3U) == 0U) ppu->vram[i] = 0U;
        ppu->background_mode = trial % 4U == 0U ? 3U : 2U;
        ppu->main_screen = 0x13U;
        ppu->bg2_tile_size_16 = (trial & 1U) != 0U;
        ppu->bg2_screen_size = static_cast<std::uint8_t>(next(state) & 3U);
        ppu->bg2_character_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x1000U);
        ppu->bg2_screen_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x0800U);
        const auto scroll_x = static_cast<std::int32_t>(next(state) & 0x3ffU) - 512;
        const auto scroll_y = static_cast<std::int32_t>(next(state) & 0x3ffU) - 512;
        ppu->bg2_horizontal_offsets_enabled = (trial % 5U) == 1U;
        ppu->bg2_scanline_scroll_enabled = (trial % 7U) == 2U;
        ppu->bg2_vertical_offsets_enabled = ppu->background_mode == 2U && (trial % 3U) != 1U;
        for (auto& value : ppu->bg2_horizontal_offsets)
            value = static_cast<std::int16_t>(next(state) & 0x7ffU);
        for (auto& value : ppu->bg2_scanline_scroll_y)
            value = static_cast<std::int16_t>(next(state) & 0x7ffU);
        for (const auto pass : {TilePriorityPass::all, TilePriorityPass::low, TilePriorityPass::high}) {
            for (const bool coverage : {false, true}) {
                Framebuffer runs(256U, 224U), pixels(256U, 224U);
                for (std::size_t i = 0; i < runs.pixels().size(); ++i)
                    runs.pixels()[i] = pixels.pixels()[i] = static_cast<std::uint8_t>(next(state));
                if (coverage) { runs.begin_write_coverage(); pixels.begin_write_coverage(); }
                ppu->mosaic = 0U;
                renderer.draw_bg2(*ppu, scroll_x, scroll_y, runs, pass);
                ppu->mosaic = 0x02U; // BG2 enabled, size 1
                renderer.draw_bg2(*ppu, scroll_x, scroll_y, pixels, pass);
                if (runs.pixels() != pixels.pixels()) {
                    for (std::size_t i = 0; i < runs.pixels().size(); ++i)
                        if (runs.pixels()[i] != pixels.pixels()[i]) {
                            std::printf("trial %u pass %d cov %d x=%zu y=%zu runs=%u pixels=%u mode=%u t16=%d hofs=%d vofs=%d opt=%d size=%u\n",
                                trial, int(pass), int(coverage), i % 256U, i / 256U, runs.pixels()[i], pixels.pixels()[i],
                                ppu->background_mode, int(ppu->bg2_tile_size_16), int(ppu->bg2_horizontal_offsets_enabled),
                                int(ppu->bg2_scanline_scroll_enabled), int(ppu->bg2_vertical_offsets_enabled), ppu->bg2_screen_size);
                            return 1;
                        }
                }
                if (coverage) {
                    const auto a = runs.write_coverage(), b = pixels.write_coverage();
                    assert(a.size() == b.size());
                    for (std::size_t i = 0; i < a.size(); ++i) assert(a[i] == b[i]);
                }
                compared += static_cast<unsigned>(runs.pixels().size());
            }
        }
    }
    std::printf("bg2 runs: %u pixels match the per-pixel renderer\n", compared);

    // The 400x240 run path (wide, 4:3 and GPU-fallback gameplay) against the
    // same per-pixel renderer, including Mode 2 column offsets and the
    // rolled-ground continuation of the lower rows.
    unsigned wide_compared = 0U;
    for (unsigned trial = 0U; trial < 240U; ++trial) {
        auto ppu = std::make_unique<starfox::simulation::SnesPpuState>();
        for (auto& byte : ppu->vram) byte = static_cast<std::uint8_t>(next(state));
        if (trial % 3U == 0U)
            for (std::size_t i = 0; i < ppu->vram.size(); ++i)
                if ((next(state) & 3U) == 0U) ppu->vram[i] = 0U;
        ppu->background_mode = trial % 5U == 0U ? 1U : trial % 5U == 1U ? 3U : 2U;
        ppu->main_screen = 0x13U;
        ppu->bg2_tile_size_16 = (trial & 1U) != 0U;
        ppu->bg2_screen_size = static_cast<std::uint8_t>(next(state) & 3U);
        ppu->bg2_character_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x1000U);
        ppu->bg2_screen_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x0800U);
        const auto scroll_x = static_cast<std::int32_t>(next(state) & 0xfffU) - 2048;
        const auto scroll_y = static_cast<std::int32_t>(next(state) & 0xfffU) - 2048;
        ppu->bg2_horizontal_offsets_enabled = (trial % 4U) == 1U;
        ppu->bg2_scanline_scroll_enabled = ppu->background_mode != 1U && (trial % 7U) == 2U;
        ppu->bg2_vertical_offsets_enabled = ppu->background_mode == 2U && (trial % 4U) != 3U;
        for (auto& value : ppu->bg2_horizontal_offsets)
            value = static_cast<std::int16_t>(static_cast<std::int32_t>(next(state) & 0xfffU) - 2048);
        for (auto& value : ppu->bg2_scanline_scroll_y)
            value = static_cast<std::int16_t>(static_cast<std::int32_t>(next(state) & 0xfffU) - 2048);
        // Mode 2 column offsets: word 0x2fa0 onwards, bit 14 marks valid.
        for (unsigned i = 0U; i < 32U; ++i) {
            const auto word = static_cast<std::uint16_t>((next(state) & 0x1fffU)
                | ((next(state) & 3U) != 0U ? 0x4000U : 0U));
            ppu->vram[(0x2fa0U + i) * 2U] = static_cast<std::uint8_t>(word);
            ppu->vram[(0x2fa0U + i) * 2U + 1U] = static_cast<std::uint8_t>(word >> 8U);
        }
        for (const auto pass : {TilePriorityPass::all, TilePriorityPass::low, TilePriorityPass::high}) {
            for (const bool coverage : {false, true}) {
                Framebuffer runs(400U, 240U), pixels(400U, 240U);
                for (std::size_t i = 0; i < runs.pixels().size(); ++i)
                    runs.pixels()[i] = pixels.pixels()[i] = static_cast<std::uint8_t>(next(state));
                if (coverage) { runs.begin_write_coverage(); pixels.begin_write_coverage(); }
                ppu->mosaic = 0U;
                renderer.draw_bg2(*ppu, scroll_x, scroll_y, runs, pass, 72, true, true, false, 0U, {}, 8);
                ppu->mosaic = 0x02U;
                renderer.draw_bg2(*ppu, scroll_x, scroll_y, pixels, pass, 72, true, true, false, 0U, {}, 8);
                if (runs.pixels() != pixels.pixels()) {
                    for (std::size_t i = 0; i < runs.pixels().size(); ++i)
                        if (runs.pixels()[i] != pixels.pixels()[i]) {
                            std::printf("wide trial %u pass %d cov %d x=%zu y=%zu runs=%u pixels=%u mode=%u\n",
                                trial, int(pass), int(coverage), i % 400U, i / 400U,
                                runs.pixels()[i], pixels.pixels()[i], ppu->background_mode);
                            return 1;
                        }
                }
                if (coverage) {
                    const auto a = runs.write_coverage(), b = pixels.write_coverage();
                    assert(a.size() == b.size());
                    for (std::size_t i = 0; i < a.size(); ++i) assert(a[i] == b[i]);
                }
                wide_compared += static_cast<unsigned>(runs.pixels().size());
            }
        }
    }
    std::printf("bg2 wide runs: %u pixels match the per-pixel renderer\n", wide_compared);
}
