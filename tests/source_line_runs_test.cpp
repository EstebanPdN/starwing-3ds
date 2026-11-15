#include "starfox/render/source_line_runs.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>

int main() {
    std::mt19937 random(0x165c816U);
    std::uint64_t pixels{},rectangles{};
    for (unsigned trial=0;trial<16384U;++trial) {
        const int ax=int(random()%129U)-32,ay=int(random()%97U)-24;
        const int bx=int(random()%129U)-32,by=int(random()%97U)-24;
        const int scale=1+trial%4,thickness=scale*(1+(trial/4)%4),offset=(thickness-1)/2;
        const auto even=std::uint8_t(trial%256),odd=std::uint8_t((trial*3)%256);
        std::array<std::uint8_t,64*48> expected{},actual{};
        expected.fill(253);actual.fill(253);
        const auto paint=[&](auto& image,int x,int y) {
            if (unsigned(x)<64U && unsigned(y)<48U)
                image[y*64+x]=(((x/scale)^(y/scale))&1)?odd:even;
        };
        // Independent pixel-at-a-time reference from MDRAWC's original
        // renderer. Reversed directions, half-slope ties, zero paint,
        // thick lines and all four clip boundaries remain observable.
        int x=ax,y=ay;
        const int dx=std::abs(bx-ax),dy=std::abs(by-ay),sx=ax<bx?1:-1,sy=ay<by?1:-1;
        const auto plot=[&] {
            ++pixels;
            for(int row=0;row<thickness;++row)
                for(int col=0;col<thickness;++col)paint(expected,x+col-offset,y+row-offset);
        };
        if(dx>=dy) {
            int error=dx>>1;
            for(int count=dx+1;count;--count) {
                plot();error-=dy;if(error<0){error+=dx;y+=sy;}x+=sx;
            }
        } else {
            int error=dy>>1;
            for(int count=dy+1;count;--count) {
                plot();error-=dx;if(error<0){error+=dy;x+=sx;}y+=sy;
            }
        }
        starfox::render::source_line_runs(ax,ay,bx,by,thickness,[&](int left,int top,int right,int bottom) {
            ++rectangles;
            for(int yy=std::max(0,top);yy<std::min(48,bottom);++yy)
                for(int xx=std::max(0,left);xx<std::min(64,right);++xx)paint(actual,xx,yy);
        });
        if(actual!=expected) {std::fprintf(stderr,"source line differs at trial %u\n",trial);return 1;}
    }
    std::printf("PASS: 16384 clipped/thick/dithered lines, %llu point writes -> %llu rectangles\n",
        (unsigned long long)pixels,(unsigned long long)rectangles);
}
