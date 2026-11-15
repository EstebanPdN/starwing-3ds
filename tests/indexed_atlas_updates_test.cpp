#include "../platform/3ds/source/indexed_atlas_updates.hpp"
#include <cstdio>
#include <random>
#include <stdexcept>
using namespace starfox;
int main() try {
    platform_3ds::IndexedAtlasUpdates updates;
    std::mt19937 random(0xa71a5006U);
    std::uint64_t checked{},changed_blocks{};
    for(unsigned trial=0;trial<32;++trial) {
        const unsigned widths[]{64,256,512,1536};
        const unsigned width=widths[trial%4],height=trial%3?64:256;
        render::Framebuffer frame(width+16,height);
        for(auto& v:frame.pixels())v=std::uint8_t(random()%32U);
        std::array<std::uint32_t,256> colours{};
        for(unsigned i=1;i<256;++i)colours[i]=random();
        std::vector<std::uint32_t> gpu(std::size_t(width)*height,0xdeadbeefU);
        for(unsigned step=0;step<12;++step) {
            std::array<bool,256> changed{};
            bool source_changed=true;
            if(step==1)frame.set(width+7,3,99); // Outside the retained crop.
            if(step==2){colours[97]^=0x12345678;changed[97]=true;source_changed=false;}
            if(step>=3 && step<=6)for(unsigned i=0;i<7;++i)frame.set(random()%width,random()%height,random()%128);
            if(step==5 || step==7){colours[17]^=0x23456789;changed[17]=true;}
            if(step==8)frame.clear();
            if(step==10)frame.set(width-1,height-1,97);
            if(!updates.update(frame,width,height,source_changed,changed))throw std::runtime_error("valid atlas rejected");
            if(step==11)updates.mark_all(); // Failed clean/allocation retry.
            unsigned dirty{};
            for(unsigned ty=0;ty<height/8;++ty)for(unsigned tx=0;tx<width/8;++tx)if(updates.dirty(tx,ty)) {
                ++dirty;
                for(unsigned y=ty*8;y<ty*8+8;++y)for(unsigned x=tx*8;x<tx*8+8;++x)
                    gpu[std::size_t(y)*width+x]=colours[updates.row(y)[x]];
            }
            if((step==1 || step==2 || step==9) && dirty)throw std::runtime_error("unchanged atlas was dirtied");
            std::array<bool,256> used{};
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
                const auto index=frame.get(x,y);used[index]=true;++checked;
                if(gpu[std::size_t(y)*width+x]!=colours[index])throw std::runtime_error("partial update differs from full conversion");
            }
            for(unsigned i=0;i<256;++i)if(updates.used(i)!=used[i])throw std::runtime_error("palette usage differs");
            if(updates.empty()!=std::none_of(used.begin()+1,used.end(),[](bool v){return v;}))throw std::runtime_error("empty layer differs");
            changed_blocks+=dirty;
        }
        std::array<bool,256> none{};
        if(updates.update(frame,width+32,height,true,none) || updates.update(frame,width-1,height,true,none))
            throw std::runtime_error("invalid crop accepted");
    }
    std::printf("PASS: exact partial atlas updates, %llu pixels / %llu dirty blocks; palette, crop, resize, erase and retry\n",
        (unsigned long long)checked,(unsigned long long)changed_blocks);
} catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
