#include "../platform/3ds/source/bg2_map_cache.hpp"
#include <memory>
#include <stdexcept>
#include <cstdio>

using namespace starfox;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
int main() try {
    auto p=std::make_unique<simulation::SnesPpuState>();
    p->bg2_character_base=0x1000U;p->bg2_screen_base=0x6000U;
    auto cache=std::make_unique<platform_3ds::Bg2MapSourceCache>();
    require(!cache->matches(*p),"uninitialized map cache matched");
    for(unsigned size=0U;size<4U;++size) {
        p->bg2_screen_size=size;cache->remember(*p);
        require(cache->matches(*p),"fresh map cache failed");
        p->vram[0x5f40U]^=0x41U;p->bg2_horizontal_offsets[0]++;
        require(cache->matches(*p),"roll/HDMA incorrectly invalidated map image");
        p->vram[0x2000U]^=0x80U;
        require(!cache->matches(*p),"referenced character change was missed");
        cache->remember(*p);p->vram[0xc000U]^=0x40U;
        require(!cache->matches(*p),"map entry change was missed");
        p->vram[0xc000U]^=0x40U;cache->remember(*p);
        p->bg2_character_base++;
        require(!cache->matches(*p),"character base change was missed");
        p->bg2_character_base--;
    }
    p->bg2_screen_base=0x7f00U;cache->remember(*p);
    require(cache->matches(*p),"wrapped map range failed");
    p->vram[4U]^=1U;require(!cache->matches(*p),"wrapped map change missed");
    p->bg2_tile_size_16=true;cache->remember(*p);
    require(!cache->matches(*p),"16 pixel fallback incorrectly cached");
    cache->reset();require(!cache->matches(*p),"reset failed");
    require(!cache->all_opaque(),"reset retained an opacity certificate");
    *p = {};
    p->bg2_character_base = 0x7ff8U;
    p->bg2_screen_base = 0x6000U;
    // Referenced character zero wraps through the end of VRAM. A nonzero
    // bit in ANY of its four planes makes that pixel opaque.
    for (unsigned row = 0U; row < 8U; ++row)
        p->vram[(0xfff0U + row * 2U) & 65535U] = 255U;
    cache->remember(*p);
    require(cache->matches(*p) && cache->all_opaque(),"opaque wrapped character was missed");
    p->vram[0x0100U] ^= 1U;
    require(cache->matches(*p) && cache->all_opaque(),"unused character invalidated opacity");
    p->vram[0xfff0U] &= 0x7fU;
    require(!cache->matches(*p),"transparent referenced pixel did not invalidate certificate");
    cache->remember(*p);
    require(!cache->all_opaque(),"transparent map was certified opaque");
    p->vram[0U] = 0x80U; // Plane two covers that same wrapped pixel.
    cache->remember(*p);
    require(cache->all_opaque(),"higher bitplane opacity was missed");
    p->vram[0xc000U] = 1U;
    require(!cache->matches(*p),"new transparent map character was missed");
    cache->remember(*p);
    require(!cache->all_opaque(),"new transparent character kept opaque certificate");
    p->bg2_tile_size_16 = true;
    cache->remember(*p);
    require(!cache->all_opaque(),"unsupported tile size retained opacity certificate");
    std::puts("BG2 source map: roll reuse, referenced characters, map edits, wrap, base and tile-size invalidation passed");
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
