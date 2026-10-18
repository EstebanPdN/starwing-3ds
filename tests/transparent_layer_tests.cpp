#include "starfox/render/framebuffer.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace starfox::render;
static void compare(const Framebuffer& a,const Framebuffer& b) {
    if(a.pixels()!=b.pixels() || a.layer_tags()!=b.layer_tags()
        || !std::equal(a.write_coverage().begin(),a.write_coverage().end(),
            b.write_coverage().begin(),b.write_coverage().end()))
        throw std::runtime_error("transparent assignment differs in pixels, tags or ownership");
}
int main() try {
    unsigned random=0x78a6532f;
    auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned trial=0;trial<3000;++trial) {
        const auto scale=next()%3+1;
        const auto w=next()%100+1,h=next()%80+1;
        Framebuffer src(w,h,scale),a(w,h,scale),b(w,h,scale);
        src.enable_layer_tags(trial&1);a.enable_layer_tags(trial&2);b.enable_layer_tags(trial&2);
        for(auto& pixel:src.pixels())pixel=(next()%3)?next()%256:0;
        for(auto& tag:src.layer_tags())tag=next()%5;
        a.clear(147);b.clear(147);
        if(trial&4){a.begin_write_coverage();b.begin_write_coverage();a.clear(1);b.clear(1);}
        a.end_write_coverage();a.clear(0);
        if(trial&4)a.begin_write_coverage();
        composite_transparent_layer(src,a,{});
        b.assign_transparent_layer(src,trial&4);
        compare(a,b);
        // Explicit black foreground after world composition must own the cell.
        if(trial&4){a.set(0,0,0);b.set(0,0,0);compare(a,b);
            if(b.write_coverage()[0]!=1)throw std::runtime_error("black foreground lost coverage");}
    }
    std::cout<<"PASS 3000 transparent world cases: pixels, tags, coverage, scales and black foreground\n";
    for(unsigned pattern=0;pattern<5;++pattern) {
        Framebuffer src(400,240),a(400,240),b(400,240);
        for(unsigned y=0;y<240;++y)for(unsigned x=0;x<400;++x) {
            const bool ink=pattern==0?false:pattern==1?true:pattern==2?((x+y)%2):pattern==3?((x-unsigned(200-y/2))<y):next()%200==0;
            src.pixels()[y*400+x]=ink?100:0;
        }
        const auto run=[&](bool optimized,Framebuffer& dst){auto t=std::chrono::steady_clock::now();
            for(int i=0;i<2000;++i){dst.end_write_coverage();if(optimized)dst.assign_transparent_layer(src,true);
                else {dst.clear(0);dst.begin_write_coverage();composite_transparent_layer(src,dst,{});}}
            return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();};
        for(unsigned rep=0;rep<3;++rep){const auto before=run(false,a),after=run(true,b);compare(a,b);
            std::cout<<"pattern="<<pattern<<" baseline_ms="<<before<<" assignment_ms="<<after<<'\n';}
    }
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
