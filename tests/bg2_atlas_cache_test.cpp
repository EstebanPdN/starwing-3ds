#include "../platform/3ds/source/bg2_atlas_cache.hpp"
#include "../platform/3ds/source/bg2_plan.hpp"
#include <cstdio>
#include <memory>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>
using starfox::simulation::SnesPpuState;
using starfox::platform_3ds::Bg2AtlasSourceCache;
static void check(bool good) { if (!good) throw std::runtime_error("BG2 atlas invalidation mismatch"); }
int main(int argc, char** argv) try {
    auto ppu = std::make_unique<SnesPpuState>();
    auto cache = std::make_unique<Bg2AtlasSourceCache>();
    std::mt19937 random(70);
    unsigned checks = 0;
    for (unsigned pass = 0; pass < 128; ++pass) {
        for (auto& b : ppu->vram) b = random();
        ppu->bg2_character_base = random();
        cache->reset();
        std::array<bool, 65536> used{};
        for (unsigned n = 0; n < 64; ++n) {
            const auto c = random() & 1023U;
            cache->remember(*ppu, c);
            for (unsigned b = 0; b < 32; ++b)
                used[(ppu->bg2_character_base * 2U + c * 32U + b) & 65535U] = true;
        }
        check(cache->matches(*ppu));
        for (unsigned n = 0; n < 256; ++n) {
            const auto addr = random() & 65535U;
            ppu->vram[addr] ^= 0x80;
            check(cache->matches(*ppu) == !used[addr]);
            ppu->vram[addr] ^= 0x80;
            ++checks;
        }
    }
    // Force wrap within a remembered character, including both sides of VRAM.
    cache->reset(); ppu->bg2_character_base = 0x7fff;
    cache->remember(*ppu, 0);
    for (unsigned addr : {65534U, 65535U, 0U, 29U}) {
        ppu->vram[addr] ^= 1; check(!cache->matches(*ppu));
        ppu->vram[addr] ^= 1; check(cache->matches(*ppu));
    }
    unsigned retained = 0, transitions = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        in.read(reinterpret_cast<char*>(ppu.get()), sizeof(*ppu)); check(bool(in));
        if (i > 1) { retained += cache->matches(*ppu); ++transitions; }
        cache->reset();
        std::vector<starfox::platform_3ds::Bg2Rect> plan;
        check(starfox::platform_3ds::plan_bg2_rects(*ppu, ppu->bg2_scroll_x,
            ppu->bg2_scroll_y, plan, 16383, 400, 240, 72, 8));
        for (const auto& r : plan) if (!r.solid) cache->remember(*ppu, r.character);
        check(cache->matches(*ppu));
        // Real roll table update must be classified solely by actual overlap.
        auto changed = *ppu;
        changed.vram[0x5f40] ^= 1;
        bool overlaps = false;
        for (const auto& r : plan) if (!r.solid)
            for (unsigned b = 0; b < 32; ++b)
                overlaps |= ((ppu->bg2_character_base * 2U + r.character * 32U + b) & 65535U) == 0x5f40;
        check(cache->matches(changed) == !overlaps);
    }
    std::printf("PASS %u random VRAM mutations + wrap/reset + %d physical snapshots; unchanged decoded patterns across physical transitions %u/%u; fixed cache=%zu bytes\n", checks, argc-1, retained, transitions, sizeof(*cache));
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
