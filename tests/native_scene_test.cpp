#include "native_engine/scene.hpp"
#include "starfox/render/gpu_raster.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace starfox;
using platform_3ds::native_engine::Scene;
void require(bool test,const char* message) { if (!test) throw std::runtime_error(message); }
int main() try {
    std::mt19937 random(0x5f58b);
    std::size_t rectangles{},pixels{};
    for (unsigned trial=0;trial<96;++trial) {
        render::RasterCommands batch;batch.reset(64,48);
        batch.texels.resize(256);for (auto& v:batch.texels) v=std::uint8_t(random()%16);
        for (unsigned n=0;n<64;++n) {
            render::RasterCommand c;
            c.left=int(random()%90)-15;c.right=c.left+1+random()%30;
            c.top=int(random()%70)-10;c.bottom=c.top+1+random()%20;
            c.even=random()%256;c.odd=random()%256;c.dither=random()%2;
            c.reserved0=1+random()%4;
            if (n%4==1) { c.textured=1;c.u_mask=c.v_mask=15;c.du=int(random()%1024)-512;c.dv=int(random()%1024)-512;c.u=random()%65536;c.v=random()%65536;c.reserved0=random()%16;c.reserved1=random()%16;c.colour_base=random()%240; }
            else if(n%4==2) {c.reserved1=1;c.u=c.left+3;c.v=c.right-2;}
            else if(n%4==3) {c.reserved1=4;c.u_mask=8;c.v_mask=32;}
            batch.add(c);
        }
        Scene scene;require(scene.compile(batch),"Unexpected capacity failure");
        render::Framebuffer expected(64,48),actual(64,48);
        render::replay_raster_commands(batch,expected,nullptr);
        for (const auto& r:scene.rectangles()) for(int y=r.top;y<r.bottom;++y) for(int x=r.left;x<r.right;++x)
            actual.set(x,y,(((x/r.scale)^(y/r.scale))&1)?r.odd:r.even);
        require(actual.pixels()==expected.pixels(),"Native scene changed ordered pixel output");
        rectangles+=scene.rectangles().size();pixels+=actual.pixels().size();
    }
    render::RasterCommands full;full.reset(8,8);
    render::RasterCommand c;c.right=c.bottom=1;
    for (unsigned n=0;n<=Scene::capacity;++n) { c.left=n&1;c.right=c.left+1;c.even=c.odd=n&255;full.add(c); }
    Scene scene;require(!scene.compile(full)&&scene.stats().overflow,"Overflow did not request full fallback");
    full.reset(8,8);require(scene.compile(full)&&!scene.stats().overflow&&scene.rectangles().empty(),"Scene reset retained stale draws");
    std::cout<<"Native scene: "<<pixels<<" pixels, "<<rectangles<<" rectangles; order, dither, texture, mask, black, bounds and overflow verified\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
