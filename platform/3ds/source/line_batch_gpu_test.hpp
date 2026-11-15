#pragma once
#include "display_3ds.hpp"
#include "starfox/render/source_line_runs.hpp"
#include "top_viewport.hpp"
#include <algorithm>
#include <cstdio>
// Eight on-console GPU cases for source line batches, run by run_gpu_self_test
// (gpu_self_test.cpp) as its last suite. 3DS only.

namespace starfox::platform_3ds {
// Each case draws 192 seeded random lines (thickness and dither scale 1-4) as
// source_line_runs rectangles through the native presenter and compares every
// top-screen pixel with a CPU Bresenham reference. Appends JSON lines to
// `report` (cases 76-83, up to eight mismatches each) and adds to the counters.
inline void run_line_batch_gpu_tests(Display3ds& display,FILE* report,
    unsigned& completed,unsigned& failed) {
    std::uint32_t state=0x165c816U;
    const auto random=[&]{state=state*1664525U+1013904223U;return state;};
    render::Palette256 palette{};
    for(unsigned i=0;i<256;++i)palette[i]={std::uint8_t(i*29),std::uint8_t(i*13),std::uint8_t(i*7),255};
    for(unsigned test=0;test<8;++test) {
        if(!aptMainLoop() || !display.wait_gpu_idle(500)){++failed;break;}
        const bool wide=(test&1U)==0;
        display.set_wide(wide);
        render::Framebuffer background(400,240),hud(400,240),expected(400,240);
        hud.begin_write_coverage();hud.end_write_coverage();
        render::RasterCommands batch;batch.reset(400,240);
        unsigned points{};
        for(unsigned n=0;n<192;++n) {
            int x=int(random()%440)-20,y=int(random()%280)-20;
            const int ax=x,ay=y,bx=x+int(random()%121)-60,by=y+int(random()%121)-60;
            const int scale=1+(n%4),thickness=1+((n/4)%4),offset=(thickness-1)/2;
            render::RasterCommand c;c.even=std::uint8_t(n*23);c.odd=std::uint8_t(n*47);
            c.dither=true;c.reserved0=scale;
            render::source_line_runs(x,y,bx,by,thickness,[&](int l,int t,int r,int b) {
                c.left=l;c.top=t;c.right=r;c.bottom=b;batch.add(c);
            });
            const int dx=std::abs(bx-ax),dy=std::abs(by-ay),sx=ax<bx?1:-1,sy=ay<by?1:-1;
            const auto plot=[&] {
                ++points;
                for(int row=0;row<thickness;++row)for(int col=0;col<thickness;++col) {
                    const int xx=x+col-offset,yy=y+row-offset;
                    const auto index=std::uint8_t((((xx/scale)^(yy/scale))&1)?c.odd:c.even);
                    expected.set(xx,yy,index);
                }
            };
            if(dx>=dy){int error=dx>>1;for(int n=dx+1;n;--n){plot();error-=dy;if(error<0){error+=dx;y+=sy;}x+=sx;}}
            else {int error=dy>>1;for(int n=dy+1;n;--n){plot();error-=dx;if(error<0){error+=dy;x+=sx;}y+=sy;}}
        }
        native_engine::Scene scene;
        const bool drawn=scene.compile(batch) && display.present_native(background,hud,palette,scene);
        bool readable=drawn && display.wait_gpu_idle(500);
        const auto* fb=reinterpret_cast<const std::uint16_t*>(display.top_framebuffer());
        readable=readable && fb && R_SUCCEEDED(GSPGPU_InvalidateDataCache(fb,192000U));
        const auto view=top_viewport(400,240,wide);
        unsigned different{};
        for(unsigned y=0;y<240;++y)for(unsigned x=0;x<400;++x) {
            std::uint16_t want{};
            if(wide || (x>=40 && x<360)) {
                const auto c=palette[expected.get(view.sample_x(x),view.sample_y(y))];
                want=std::uint16_t(((c.r>>3)<<11)|((c.g>>2)<<5)|(c.b>>3));
            }
            if(!readable || fb[x*240+239-y]!=want) {
                if(readable && different<8U)std::fprintf(report,
                    "{\"line_mismatch\":%u,\"x\":%u,\"y\":%u,\"expected\":%u,\"actual\":%u}\n",
                    test,x,y,unsigned(want),unsigned(fb[x*240+239-y]));
                ++different;
            }
        }
        ++completed;failed+=!readable || different!=0;
        std::fprintf(report,"{\"case\":%u,\"fixture\":\"source_line_batch\",\"different_pixels\":%u,"
            "\"point_writes\":%u,\"commands\":%u,\"gpu_rectangles\":%u,\"drawn\":%s}\n",
            76U+test,different,points,unsigned(batch.commands.size()),unsigned(scene.rectangles().size()),drawn?"true":"false");
        std::fflush(report);
    }
}
}
