#include "../platform/3ds/source/bg3_atlas_cache.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
int main() try {
    using namespace starfox;
    simulation::SnesPpuState ppu{};
    platform_3ds::Bg3AtlasSourceCache cache;
    unsigned checks=0;
    const auto check=[&](bool value){++checks;if(!value)
        throw std::runtime_error("BG3 source cache mismatch at check " + std::to_string(checks));};
    check(!cache.matches(ppu));
    for (unsigned size=0; size<4; ++size) for (bool large : {false,true})
    for (unsigned base : {0U, 0x7ffeU}) {
        ppu={}; ppu.bg3_character_base=base; ppu.bg3_screen_base=0x7ffcU;
        ppu.bg3_screen_size=size; ppu.bg3_tile_size_16=large;
        // Map and characters cross the 64 KiB VRAM boundary and may overlap.
        for(unsigned i=0;i<ppu.vram.size();++i) ppu.vram[i]=std::uint8_t(i*13U+(i>>4U));
        cache.capture(ppu); check(cache.matches(ppu));
        ppu.bg3_scroll_x=123; ppu.bg3_scroll_y=-42; ppu.mosaic=0xf4;
        check(cache.matches(ppu)); // scrolling reads the already built atlas
        const auto start=(unsigned(ppu.bg3_screen_base)*2U)&65535U;
        const auto bytes=2048U*((size&1)?2U:1U)*((size&2)?2U:1U);
        for(unsigned offset : {0U,7U,bytes-1U}) {
            ppu.vram[(start+offset)&65535U]^=1; check(!cache.matches(ppu));
            ppu.vram[(start+offset)&65535U]^=1; check(cache.matches(ppu));
        }
        const auto tile=(unsigned(ppu.vram[start]) | (unsigned(ppu.vram[(start+1)&65535])<<8))&1023U;
        for(unsigned offset : {0U,1U,16U,17U}) {
            if (!large && offset) continue;
            const auto address=(base*2U+((tile+offset)&1023U)*16U)&65535U;
            ppu.vram[address]^=1; check(!cache.matches(ppu));
            ppu.vram[address]^=1; check(cache.matches(ppu));
        }
        ++ppu.bg3_character_base; check(!cache.matches(ppu)); --ppu.bg3_character_base;
        ppu.bg3_tile_size_16=!large; check(!cache.matches(ppu)); ppu.bg3_tile_size_16=large;
    }
    ppu={}; ppu.bg3_character_base=0; ppu.bg3_screen_base=0x1000; cache.capture(ppu);
    ppu.vram[0x7000]=1; check(cache.matches(ppu)); // unrelated tile/OBJ writes
    ppu.vram[15]=1; check(!cache.matches(ppu));
    std::puts("BG3 dependency cache: map, characters, wrap, 16x16 and unrelated writes pass");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
