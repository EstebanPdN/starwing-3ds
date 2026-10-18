#include "../platform/3ds/source/native_canvas.hpp"
#include "starfox/render/background_renderer.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
using namespace starfox;
void require(bool test,const char* message) {if(!test)throw std::runtime_error(message);}
int main() try {
    std::mt19937 random(0x804003d5U);
    auto p=std::make_unique<simulation::SnesPpuState>();
    for(unsigned i=0;i<32;++i)p->cgram[i]=static_cast<std::uint16_t>((i%16)|((i%12)<<5)|((i%9)<<10));
    p->cgram[10]=static_cast<std::uint16_t>(29U|(21U<<5)|(11U<<10));
    p->cgram[22]=static_cast<std::uint16_t>(24U|(24U<<5)|(24U<<10));
    render::Framebuffer low(256,256),high(256,256),atlas(0,0),atlash(0,0);
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {
        if(y>=112) {
            if(random()%100<4)low.set(x,y,22);
            else if(y<128)low.set(x,y,13+(x+y)%3);
        } else if(random()%5==0)low.set(x,y,5+(x+y)%3);
    }
    for(unsigned y=48;y<56;++y)for(unsigned x=48;x<56;++x)high.set(x,y,10);
    platform_3ds::build_native_panorama_atlas(low,high,atlas,atlash,*p);
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {
        require(atlas.get(x+platform_3ds::native_panorama_padding,y)==low.get(x,y),"Authored atlas low plane changed");
        require(atlash.get(x+platform_3ds::native_panorama_padding,y)==high.get(x,y),"Authored atlas high plane changed");
    }
    unsigned stars{};
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<atlas.width();++x)
        if(x<platform_3ds::native_panorama_padding||x>=platform_3ds::native_panorama_padding+256) {
            if(y<112)require(atlash.get(x,y)!=10,"Unique bright art repeated into extension");
            else stars+=atlas.get(x,y)==22;
        }
    require(stars>0,"Expanded lower star field is empty");
    render::Framebuffer a(400,240),ah(400,240),b(400,240),bh(400,240);
    std::uint64_t translated{};
    for(int shift=-511;shift<511;++shift) {
        p->bg3_scroll_x=static_cast<std::int16_t>(shift);
        p->bg3_scroll_y=static_cast<std::int16_t>(shift%31);
        platform_3ds::scroll_native_panorama_atlas(atlas,atlash,a,ah,*p);
        ++p->bg3_scroll_x;
        platform_3ds::scroll_native_panorama_atlas(atlas,atlash,b,bh,*p);
        for(unsigned y=0;y<240;++y)for(unsigned x=0;x<399;++x) {
            require(a.get(x+1,y)==b.get(x,y),"Low panorama remeshes while panning");
            require(ah.get(x+1,y)==bh.get(x,y),"High panorama remeshes while panning");
            ++translated;
        }
    }
    // Native and slow BG3 decoders must agree for complete stationary maps,
    // including 16x16 characters and both priority planes.
    render::BackgroundRenderer bg;
    unsigned decoder_cases{},bg2_cases{};
    for(unsigned trial=0;trial<32;++trial) {
        for(auto& v:p->vram)v=static_cast<std::uint8_t>(random());
        p->main_screen=6;p->background_mode=1;p->bg3_screen_size=trial&3;
        p->bg3_tile_size_16=(trial&4)!=0;p->bg3_scroll_x=0;p->bg3_scroll_y=0;
        p->mosaic=(trial&8)?0x34:0;
        const auto edge=p->bg3_tile_size_16?16:8;
        const auto w=((trial&1)?64:32)*edge,h=((trial&2)?64:32)*edge;
        render::Framebuffer fast(w,h),fasth(w,h),slow(w,h),slowh(w,h);
        bg.draw_bg3_split(*p,fast,fasth);
        bg.draw_bg3(*p,slow,render::TilePriorityPass::low);
        bg.draw_bg3(*p,slowh,render::TilePriorityPass::high);
        require(fast.pixels()==slow.pixels()&&fasth.pixels()==slowh.pixels(),"Complete BG3 map decoder changed priority or tile addressing");
        ++decoder_cases;
        p->mosaic=(trial&8)?0x32:0;p->tunnel_scene=(trial&1)!=0;
        p->bg2_screen_size=trial&3;p->bg2_tile_size_16=(trial&4)!=0;
        p->bg2_scanline_scroll_enabled=true;p->bg2_horizontal_offsets_enabled=true;
        for(unsigned row=0;row<224;++row) {
            p->bg2_horizontal_offsets[row]=static_cast<std::int16_t>(random());
            p->bg2_scanline_scroll_y[row]=static_cast<std::int16_t>(random());
        }
        render::Framebuffer f(400,240),fh(400,240),s(400,240),sh(400,240);
        s.enable_layer_tags(true);sh.enable_layer_tags(true);
        bg.draw_bg2_mode1_split(*p,0,0,f,fh,72,8);
        bg.draw_bg2_mode1_split(*p,0,0,s,sh,72,8);
        require(f.pixels()==s.pixels()&&fh.pixels()==sh.pixels(),"Native BG2 tile-row and scalar decoders disagree at a clamped edge");
        ++bg2_cases;
    }
    // Mosaic sampling uses the same stationary field, including negative
    // logical screen coordinates and the extreme signed scroll values.
    for(unsigned group=2;group<=16;++group)for(int shift:{-512,-64,0,64,511}) {
        p->mosaic=static_cast<std::uint8_t>(((group-1)<<4)|4);
        p->bg3_scroll_x=static_cast<std::int16_t>(shift);p->bg3_scroll_y=-17;
        platform_3ds::scroll_native_panorama_atlas(atlas,atlash,a,ah,*p);
        for(unsigned y=0;y<240;++y)for(unsigned x=0;x<400;++x) {
            const int lx=int(x)-72,ly=std::clamp(int(y)-8,0,223);
            const auto snap=[group](int v){int r=v%int(group);return v-(r<0?r+int(group):r);};
            const auto sx=platform_3ds::native_panorama_padding+snap(lx)+shift;
            const auto sy=unsigned(snap(ly)-17)&255;
            require(a.get(x,y)==atlas.get(sx,sy)&&ah.get(x,y)==atlash.get(sx,sy),"Mosaic panorama coordinates differ from the stationary atlas");
        }
    }
    std::cout<<"PASS stationary panorama: "<<translated<<" translations per plane, "
        <<decoder_cases<<" complete BG3 map cases, "<<bg2_cases<<" BG2 native/scalar cases, 75 mosaic samples, unique upper art excluded and lower stars retained\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
