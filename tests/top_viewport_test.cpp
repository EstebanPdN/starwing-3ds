#include "../platform/3ds/source/top_viewport.hpp"
#include <cassert>
#include <cstdio>
using namespace starfox::platform_3ds;
int main() {
    const auto v=top_viewport(400,240,false);
    assert(v.x==40 && v.y==0 && v.width*3==v.height*4);
    assert(v.source_x==72 && v.source_y==8 && v.source_width==256 && v.source_height==224);
    unsigned samples=0;
    for(unsigned y=0;y<240;++y) for(unsigned x=40;x<360;++x) {
        const auto sx=v.sample_x(x), sy=v.sample_y(y);
        assert(sx>=72 && sx<328 && sy>=8 && sy<232);
        // Independent floating pixel-centre oracle for GPU crop/nearest.
        assert(sx==unsigned(72.0+(x-40+0.5)*256.0/320.0));
        assert(sy==unsigned(8.0+(y+0.5)*224.0/240.0)); ++samples;
    }
    for(const auto size:{256U,400U}) {
        const auto wide=top_viewport(size,224,true);
        for(unsigned x=wide.x;x<wide.x+wide.width;++x)
            assert(wide.sample_x(x)==x-wide.x);
    }
    const auto source=top_viewport(256,224,false);
    assert(source.source_x==0 && source.source_y==0 && source.width==320);
    std::printf("PASS: 4:3 crop, aspect, %u nearest samples and unchanged wide sampling\n",samples);
}
