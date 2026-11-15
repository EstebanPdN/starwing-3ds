#include "native_engine/material.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace starfox;
using namespace platform_3ds::native_engine;
int main() {
    render::Palette256 palette{};
    for(unsigned i=0;i<256;++i) palette[i]={std::uint8_t(i),std::uint8_t(i^85),std::uint8_t(i^170),255};
    std::array<std::uint32_t,64> tile{};
    std::array<bool,256> seen{};
    for(unsigned i=0;i<256;++i){
        const auto at=palette_texel_offset(i);assert(at<256 && !seen[at]);seen[at]=true;
        const auto x=i&15,y=i>>4;
        unsigned want=(x/8)*64+(y/8)*128;
        for(unsigned bit=0;bit<3;++bit)want|=((x>>bit)&1)<<(2*bit)|((y>>bit)&1)<<(2*bit+1);
        assert(at==want);
        const auto input=palette[i];
        const auto c=material_display_colour(input);
        assert((c.r>>3)==(input.r>>3) && (c.g>>2)==(input.g>>2) && (c.b>>3)==(input.b>>3));
        const auto vertex=material_vertex_colour(palette,i);
        const auto texture=material_colour(palette,i);
        assert((vertex&255)==c.r && ((vertex>>8)&255)==c.g && ((vertex>>16)&255)==c.b);
        assert((texture>>24)==c.r && ((texture>>16)&255)==c.g && ((texture>>8)&255)==c.b);
        assert((vertex>>24)==(i?255U:0U) && (texture&255)==(i?255U:0U));
    }
    write_material(tile.data(),0,0xffffffffU);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        unsigned at=0;
        for(unsigned bit=0;bit<3;++bit)at|=((x>>bit)&1)<<(2*bit)|((y>>bit)&1)<<(2*bit+1);
        assert(tile[at]==(((x+y)&1)?0xffffffffU:0U));
    }
    Rectangle r{1,2,399,239,1,2,3};
    const auto q=material_quad(r);
    assert(q[0].x==1 && q[0].y==2 && q[3].x==399 && q[3].y==239);
    // The same global checker coordinates at shared boundaries, including
    // scale three; a new rectangle must not restart its checker phase.
    for(unsigned scale=1;scale<=4;++scale) {
        r.scale=scale;
        const auto a=material_quad(r);
        auto neighbour=r;neighbour.left=r.right;
        const auto b=material_quad(neighbour);
        assert(a[1].u==b[0].u && a[1].v==b[0].v);
        assert(std::abs((a[3].v-a[0].v)*8*scale+237)<0.001F);
    }
    r.slanted=true;r.top_left=9;r.top_right=6390;r.bottom_left=17;r.bottom_right=6375;
    r.top_y=8;r.bottom_y=3832;
    const auto s=material_quad(r);
    assert(s[0].x==9/16.0F && s[3].x==6375/16.0F && s[3].y==3832/16.0F);
    std::puts("PASS: palette/vertex colours, palette-zero alpha, Morton layout, dither seams and subpixel corners");
}
