#include "../platform/3ds/source/screen_transfer.hpp"
#include <algorithm>
#include <vector>
#include <cassert>
int main() {
    // Compare every displayed pixel with the original nearest-sample rule,
    // including narrow crop, centred small/odd rasters and scaled storage.
    for (unsigned width:{1,255,256,320,399,400})
        for (unsigned height:{1,223,224,239,240}) for (unsigned scale:{1,2,3})
            for (bool wide:{false,true}) {
                const auto view=starfox::platform_3ds::top_viewport(width,height,wide);
                const unsigned stride=width*scale;
                std::vector<std::uint8_t> source(std::size_t(stride)*height*scale);
                for (unsigned i=0;i<source.size();++i) source[i]=(i*73U+i/17U)&255U;
                std::array<std::uint16_t,256> colours{};
                for (unsigned i=0;i<256;++i) colours[i]=i*251U;
                std::vector<std::uint16_t> actual(400*240,0xabcd);
                starfox::platform_3ds::transfer_indexed_screen(source.data(),stride,
                    scale,view,colours.data(),actual.data());
                for (unsigned y=0;y<240;++y) for (unsigned x=0;x<400;++x) {
                    std::uint16_t expected=0;
                    if (x>=view.x && x<view.x+view.width && y>=view.y && y<view.y+view.height)
                        expected=colours[source[std::size_t(view.sample_y(y))*scale*stride
                            +view.sample_x(x)*scale]];
                    assert(actual[x*240+239-y]==expected);
                }
            }
    for (const int width : {320,400}) {
        std::vector<std::uint32_t> source(width*240), actual(source.size());
        for (unsigned i=0;i<source.size();++i) source[i]=i*2654435761U;
        std::vector<std::uint16_t> rgb565(source.size());
        for(unsigned b=0;b<=15;++b) {
            starfox::platform_3ds::transfer_rgb565_screen(source.data(),rgb565.data(),width,b);
            for(int y=0;y<240;++y) for(int x=0;x<width;++x) {
                const auto c=source[y*width+x];
                const auto r=(c>>24)*b/15, g=((c>>16)&255)*b/15, blue=((c>>8)&255)*b/15;
                assert(rgb565[x*240+239-y]==((r>>3)<<11|(g>>2)<<5|(blue>>3)));
            }
            starfox::platform_3ds::transfer_rgba_screen(source.data(),actual.data(),width,b);
            for(int y=0;y<240;++y) for(int x=0;x<width;++x) {
                auto c=source[y*width+x];
                if(b!=15) c=(((c>>24)*b/15)<<24)|((((c>>16)&255)*b/15)<<16)|((((c>>8)&255)*b/15)<<8)|255;
                assert(actual[x*240+239-y]==c);
            }
        }
    }
    using namespace starfox::platform_3ds;
    std::uint32_t seed=0x2545f491U;
    const auto random=[&seed](unsigned bound) {
        seed^=seed<<13U;seed^=seed>>17U;seed^=seed<<5U;
        return seed%bound;
    };
    const auto random_area=[&] {
        const int x=int(random(340))-10,y=int(random(260))-10;
        return screen_rect(x,y,1+int(random(90)),1+int(random(60)));
    };
    // A rectangle converts exactly the pixels the whole-screen pass converts.
    {
        std::vector<std::uint32_t> source(320*240),whole(source.size()),part(source.size());
        for(unsigned i=0;i<source.size();++i) source[i]=i*2246822519U;
        for(unsigned round=0;round<400;++round) {
            const unsigned b=random(16);
            const auto area=random_area();
            transfer_rgba_screen(source.data(),whole.data(),320,b);
            std::fill(part.begin(),part.end(),0x01020304U);
            transfer_rgba_rect(source.data(),part.data(),b,area);
            FramebufferWords words{};
            words.include(area);
            for(int y=0;y<240;++y) for(int x=0;x<320;++x) {
                const bool inside=x>=area.left&&x<area.right&&y>=area.top&&y<area.bottom;
                const std::size_t word=std::size_t(x)*240U+239U-unsigned(y);
                assert(part[word]==(inside?whole[word]:0x01020304U));
                assert(!inside||(word>=words.first&&word<words.last));
            }
        }
    }
    // Damage never loses a pixel, also once its list is full.
    for(unsigned round=0;round<300;++round) {
        ScreenDamage damage;
        std::vector<std::uint8_t> expected(320*240);
        const unsigned count=1+random(14);
        for(unsigned n=0;n<count;++n) {
            const auto area=random_area();
            damage.add(area);
            for(int y=area.top;y<area.bottom;++y) for(int x=area.left;x<area.right;++x)
                expected[std::size_t(y)*320U+unsigned(x)]=1;
        }
        assert(!damage.everything()&&damage.size()<=ScreenDamage::capacity);
        std::vector<std::uint8_t> covered(320*240);
        for(const auto& area:damage) {
            assert(!area.empty()&&area.left>=0&&area.top>=0&&area.right<=320&&area.bottom<=240);
            for(int y=area.top;y<area.bottom;++y) for(int x=area.left;x<area.right;++x)
                covered[std::size_t(y)*320U+unsigned(x)]=1;
        }
        for(std::size_t i=0;i<expected.size();++i) assert(!expected[i]||covered[i]);
    }
    // A screen kept current through damage alone shows, at every
    // presentation, what a whole conversion of the image would show. The
    // device sees a word only once it was flushed; libctru alternates two
    // framebuffers (or keeps one), and sometimes other code presents.
    for(const bool double_buffered:{true,false}) {
        BottomScreenTransfer transfer;
        std::vector<std::uint32_t> image(320*240),expected(image.size());
        std::array<std::vector<std::uint32_t>,2> cpu,device;
        for(unsigned i=0;i<2;++i) {
            cpu[i].assign(image.size(),0x11111111U*(i+1U));
            device[i].assign(image.size(),0x22222222U*(i+3U));
        }
        for(auto& pixel:image) pixel=(random(1U<<16U)<<16U)|random(1U<<16U);
        unsigned current=0,brightness=15,whole=0,partial=0;
        ScreenDamage unpresented;
        for(unsigned round=0;round<6000;++round) {
            const unsigned kind=random(100);
            if(kind<3) {
                // Another screen's picture, presented through the same path.
                for(auto& pixel:image) pixel=(random(1U<<16U)<<16U)|random(1U<<16U);
                transfer.note(nullptr);
                unpresented.clear();
            } else {
                ScreenDamage damage;
                if(kind<6) {
                    for(auto& pixel:image) pixel^=0x00ff00ffU;
                    damage.add_everything();
                } else if(kind<86) {
                    const unsigned areas=1+random(3);
                    for(unsigned n=0;n<areas;++n) {
                        // Mostly HUD-sized, sometimes down to the build badge.
                        auto area=random_area();
                        if(random(5)==0) area=screen_rect(int(random(300)),200+int(random(40)),30,20);
                        for(int y=area.top;y<area.bottom;++y) for(int x=area.left;x<area.right;++x)
                            image[std::size_t(y)*320U+unsigned(x)]=(random(1U<<16U)<<16U)|random(1U<<16U);
                        damage.add(area);
                    }
                }
                if(kind>=96) brightness=random(16);
                transfer.note(&damage);
            }
            if(random(25)==0) continue; // No framebuffer this time: nothing presented.
            if(random(40)==0) {
                // One presentation by code the transfer is not told about.
                const unsigned drawing=current^(double_buffered?1U:0U);
                std::fill(cpu[drawing].begin(),cpu[drawing].end(),0xdeadbeefU);
                device[drawing]=cpu[drawing];
                current=drawing;
                if(!double_buffered) transfer.forget(); // One buffer: no order to betray it.
            }
            const unsigned drawing=current^(double_buffered?1U:0U);
            const auto words=transfer.transfer(image.data(),cpu[drawing].data(),brightness);
            if(words.last-words.first==image.size()) ++whole; else ++partial;
            std::copy(cpu[drawing].begin()+std::ptrdiff_t(words.first),
                cpu[drawing].begin()+std::ptrdiff_t(words.last),
                device[drawing].begin()+std::ptrdiff_t(words.first));
            current=drawing;
            transfer_rgba_screen(image.data(),expected.data(),320,brightness);
            draw_test_build_label(expected.data());
            assert(device[current]==expected);
        }
        assert(whole>0&&partial>whole);
    }
}
