#include "native_engine/scene.hpp"
#include "starfox/render/gpu_raster.hpp"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace starfox;
using platform_3ds::native_engine::Scene;
void require(bool test,const char* message) { if (!test) throw std::runtime_error(message); }
// A triangle as PICA fills it: corners on a sixteenth-of-a-pixel grid, a
// pixel drawn when its centre is inside. `mirrored` swaps which sides own a
// centre that falls exactly on them; a correct band must not depend on it.
struct Corner { int x,y; };
long long turn(Corner a,Corner b,Corner c) {
    return static_cast<long long>(b.x-a.x)*(c.y-a.y)-static_cast<long long>(b.y-a.y)*(c.x-a.x);
}
template<class Plot> void fill(Corner a,Corner b,Corner c,bool mirrored,int width,int height,Plot plot) {
    if (turn(a,b,c)==0) return;
    if (turn(a,b,c)<0) std::swap(b,c);
    const Corner corners[4]{a,b,c,a};
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        const Corner centre{x*16+8,y*16+8};
        bool inside=true;
        for (int edge=0;edge<3 && inside;++edge) {
            const auto from=corners[edge],to=corners[edge+1];
            const auto side=turn(from,to,centre);
            const bool left_or_top=to.y<from.y || (to.y==from.y && to.x>from.x);
            inside=side>0 || (side==0 && left_or_top!=mirrored);
        }
        if (inside) plot(x,y);
    }
}
void draw(const Scene& scene,render::Framebuffer& target,bool mirrored) {
    for (const auto& r:scene.rectangles()) {
        const auto plot=[&](int x,int y) { target.set(x,y,(((x/r.scale)^(y/r.scale))&1)?r.odd:r.even); };
        if (!r.slanted) { for(int y=r.top;y<r.bottom;++y) for(int x=r.left;x<r.right;++x) plot(x,y); continue; }
        const Corner tl{r.top_left,r.top_y},tr{r.top_right,r.top_y},br{r.bottom_right,r.bottom_y},bl{r.bottom_left,r.bottom_y};
        fill(tl,tr,br,mirrored,int(target.width()),int(target.height()),plot);
        fill(tl,br,bl,mirrored,int(target.width()),int(target.height()),plot);
    }
}
int main() try {
    std::mt19937 random(0x5f58b);
    std::size_t rectangles{},pixels{};
    for (unsigned trial=0;trial<192;++trial) {
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
    // Edge runs whose sides stay on whole pixels: every row has one answer,
    // and the slanted band must give it under either tie convention.
    std::size_t runs{};
    for (unsigned trial=0;trial<192;++trial) {
        render::RasterCommands batch;batch.reset(64,48);
        for (unsigned n=0;n<24;++n) {
            render::RasterCommand c;
            c.top=int(random()%60)-6;c.bottom=c.top+2+random()%20;
            const int rows=c.bottom-c.top,first=int(random()%70)-8,last=first+int(random()%24);
            const int steps[]{-256,0,256};
            c.u=first*256;c.v=last*256;c.du=steps[random()%3];c.dv=steps[random()%3];
            if(trial>=96) {c.u+=random()%256;c.v+=random()%256;c.du=int(random()%1537)-768;c.dv=int(random()%1537)-768;}
            if(c.v<c.u)c.v=c.u;
            if (c.v+c.dv*(rows-1)<c.u+c.du*(rows-1)) c.dv=c.du; // sides must not cross
            c.left=std::min(c.u>>8,(c.u+c.du*(rows-1))>>8);
            c.right=std::max(c.v>>8,(c.v+c.dv*(rows-1))>>8)+1;
            c.even=1+random()%255;c.odd=1+random()%255;c.dither=random()%2;c.reserved0=1+random()%3;
            c.reserved1=8;
            batch.add(c);
            if (n%3==0) { render::RasterCommand span;span.left=int(random()%60);span.right=span.left+1+random()%9;
                span.top=int(random()%44);span.bottom=span.top+1;span.even=span.odd=1+random()%255;batch.add(span); }
        }
        Scene scene;require(scene.compile(batch),"Unexpected capacity failure with edge runs");
        render::Framebuffer expected(64,48);
        render::replay_raster_commands(batch,expected,nullptr);
        for (const bool mirrored:{false,true}) {
            render::Framebuffer actual(64,48);
            draw(scene,actual,mirrored);
            require(actual.pixels()==expected.pixels(),"A slanted band left the rows of its edge run");
        }
        for (const auto& r:scene.rectangles()) runs+=r.slanted;
    }
    // Exact failing hardware fixtures from Prueba 5: slopes cross the
    // command's clipping rectangle, which is narrower than the framebuffer.
    for(unsigned test=36;test<52;++test) {
        render::RasterCommands batch;batch.reset(400,240);
        render::RasterCommand c{};c.left=10;c.right=390;c.top=10;c.bottom=225;
        c.u=25*256+int(test*17%256);c.v=270*256+int(test*39%256);
        c.du=int(test%5)*73-110;c.dv=int(test%7)*29-80;
        c.reserved1=8;c.reserved0=1;c.even=32+test;c.odd=96+test;c.dither=true;
        batch.commands.push_back(c);Scene clipped;require(clipped.compile(batch),"Clip fixture overflow");
        render::Framebuffer expected(400,240);render::replay_raster_commands(batch,expected,nullptr);
        for(bool mirrored:{false,true}) {render::Framebuffer actual(400,240);draw(clipped,actual,mirrored);
            require(actual.pixels()==expected.pixels(),"Hardware fixture escaped command clip");}
    }
    // Exercise the full certificate height and corners near its numeric
    // limits. A narrow visible viewport still clips the original 2048 rows.
    for(unsigned fixture=0;fixture<4;++fixture) {
        render::RasterCommands batch;batch.reset(1024,8);
        render::RasterCommand c{};c.left=0;c.right=1024;c.top=-1024;c.bottom=1024;
        c.reserved1=8;c.reserved0=1;c.even=42;c.odd=91;c.dither=true;
        if(fixture<2) {
            c.u=(fixture?1023:0)*256;c.v=c.u+127;
        } else {
            c.u=-1023*256;c.v=c.u+512;c.du=c.dv=255;
            if(fixture==3) {
                c.u+=c.du*2047;c.v+=c.dv*2047;c.du=c.dv=-255;
            }
        }
        batch.add(c);Scene edge;require(edge.compile(batch),"Numeric boundary fixture overflow");
        render::Framebuffer expected(1024,8);render::replay_raster_commands(batch,expected,nullptr);
        for(bool mirrored:{false,true}) {
            render::Framebuffer actual(1024,8);draw(edge,actual,mirrored);
            require(actual.pixels()==expected.pixels(),"Numeric boundary changed edge coverage");
        }
    }
    require(runs>1000,"Edge runs were not compiled as slanted bands");
    render::RasterCommands full;full.reset(8,8);
    render::RasterCommand c;c.right=c.bottom=1;
    for (unsigned n=0;n<=Scene::capacity;++n) { c.left=n&1;c.right=c.left+1;c.even=c.odd=n&255;full.add(c); }
    Scene scene;require(!scene.compile(full)&&scene.stats().overflow,"Overflow did not request full fallback");
    full.reset(8,8);require(scene.compile(full)&&!scene.stats().overflow&&scene.rectangles().empty(),"Scene reset retained stale draws");
    std::cout<<"Native scene: "<<pixels<<" pixels, "<<rectangles<<" rectangles, "<<runs<<" slanted bands; order, dither, texture, mask, black, bounds, edge runs and overflow verified\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
