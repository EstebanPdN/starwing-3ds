#include "sprite_source_cache.hpp"
#include "starfox/simulation/game_simulation.hpp"

#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>

using namespace starfox;
using Cache = platform_3ds::SpriteSourceCache;

static void require(bool condition) {
    if (!condition) throw std::runtime_error("sprite source cache mismatch");
}

static void draw(const simulation::SnesPpuState& ppu, Cache::Selection selection,
    render::Framebuffer& frame) {
    frame.end_write_coverage();
    frame.clear();
    frame.begin_write_coverage();
    render::SpriteRenderer sprites;
    simulation::MeterState meters{};
    meters.enabled = true;
    meters.boss_max_health = 128U;
    if (selection == Cache::Selection::foreground) {
        for (std::uint8_t priority = 0U; priority < 4U; ++priority)
            sprites.draw_objects(ppu, frame, priority, 0, true, false,
                nullptr, true, nullptr, false, true);
    } else {
        // Put each independently cropped HUD group in its own horizontal
        // panel, so a priority overlap cannot conceal a dependency error.
        for (auto element : {render::HudElement::lives, render::HudElement::shield,
                render::HudElement::bombs_boost, render::HudElement::boss_health}) {
            const auto panel = element == render::HudElement::boss_health
                ? 3U : static_cast<unsigned>(element);
            sprites.draw_objects(ppu, frame, std::nullopt, panel * 256U,
                false, false, nullptr, false, &meters, true, true, element);
        }
    }
    frame.end_write_coverage();
}

int main() try {
    std::mt19937 random{0x710b1ec7U};
    auto ppu = std::make_unique<simulation::SnesPpuState>();
    auto cache = std::make_unique<Cache>(Cache::Selection::foreground);
    std::uint64_t revision = 0U;
    unsigned reused = 0U;
    for (auto selection : {Cache::Selection::foreground, Cache::Selection::status_hud}) {
        cache = std::make_unique<Cache>(selection);
        render::Framebuffer expected(selection == Cache::Selection::foreground ? 256U : 1024U, 224U);
        render::Framebuffer actual(expected.width(), expected.height());
        expected.enable_layer_tags(true);
        actual.enable_layer_tags(true);
        // Every OBJ base/name-gap/size selector, including invalid size
        // selectors that the rasterizer deliberately maps to its last pair.
        for (unsigned selector = 0U; selector < 256U; ++selector) {
            ppu->object_select = static_cast<std::uint8_t>(selector);
            ppu->main_screen = 0x10U;
            for (auto& byte : ppu->vram) byte = static_cast<std::uint8_t>(random());
            for (auto& byte : ppu->oam) byte = static_cast<std::uint8_t>(random());
            // Include deliberate high-name and boundary cases on every run.
            ppu->oam[0] = 250U; ppu->oam[1] = 250U;
            ppu->oam[2] = 255U; ppu->oam[3] = 0xc1U;
            ppu->oam[4] = 119U; ppu->oam[5] = 24U;
            ppu->oam[6] = 0x3eU;
            ppu->oam[8] = 100U; ppu->oam[9] = 176U;
            ppu->oam[10] = 0x61U;
            cache->capture(*ppu, ++revision);
            draw(*ppu, selection, expected);
            for (unsigned mutation = 0U; mutation < 32U; ++mutation) {
                const auto previous = *ppu;
                if (mutation < 24U) ppu->vram[random() & 0xffffU] ^= 0xffU;
                else if (mutation < 30U) ppu->oam[random() % 544U] ^= 0xffU;
                else if (mutation == 30U) ppu->main_screen ^= 0x10U;
                else ppu->object_select ^= 0x08U;
                if (cache->matches(*ppu, ++revision)) {
                    draw(*ppu, selection, actual);
                    require(expected.pixels() == actual.pixels());
                    require(expected.layer_tags() == actual.layer_tags());
                    require(std::ranges::equal(expected.write_coverage(), actual.write_coverage()));
                    ++reused;
                }
                *ppu = previous;
                require(cache->matches(*ppu, ++revision));
            }
        }
    }
    // A moving reticle and a BG2 write must preserve the status-HUD layer,
    // while a visible HUD character mutation must invalidate it.
    *ppu = {};
    ppu->oam[0] = 8U; ppu->oam[1] = 8U; ppu->oam[2] = 189U;
    ppu->oam[4] = 100U; ppu->oam[5] = 100U; ppu->oam[6] = 0x61U;
    cache = std::make_unique<Cache>(Cache::Selection::status_hud);
    cache->capture(*ppu, ++revision);
    ++ppu->oam[4];
    ppu->vram[0x5f40U] ^= 0xffU;
    require(cache->matches(*ppu, ++revision));
    ppu->vram[(0xc000U + 189U * 32U) & 0xffffU] ^= 0xffU;
    require(!cache->matches(*ppu, ++revision));
    std::printf("16384 mutations across all 256 selectors; %u exact reusable layers; fixed cache %zu bytes\n", reused, sizeof(Cache));
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
