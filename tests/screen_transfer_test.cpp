#include "../platform/3ds/source/screen_transfer.hpp"
#include <vector>
#include <cassert>
int main() {
    for (const int width : {320,400}) {
        std::vector<std::uint32_t> source(width*240), actual(source.size());
        for (unsigned i=0;i<source.size();++i) source[i]=i*2654435761U;
        for(unsigned b=0;b<=15;++b) {
            starfox::platform_3ds::transfer_rgba_screen(source.data(),actual.data(),width,b);
            for(int y=0;y<240;++y) for(int x=0;x<width;++x) {
                auto c=source[y*width+x];
                if(b!=15) c=(((c>>24)*b/15)<<24)|((((c>>16)&255)*b/15)<<16)|((((c>>8)&255)*b/15)<<8)|255;
                assert(actual[x*240+239-y]==c);
            }
        }
    }
}
