// ROM-free device/emulator check of the production presenter and viewport.
#include "display_3ds.hpp"
#include "top_viewport.hpp"
#include <cstdio>
#include <memory>
using namespace starfox;
extern "C" {
std::uint32_t __ctru_linear_heap_size=16U*1024U*1024U;
std::uint32_t __stacksize__=512U*1024U;
}
int main() {
    // Match the launcher's bootstrap console, including its buffer mode.
    gfxInitDefault();
    consoleInit(GFX_TOP,nullptr);
    auto display=std::make_unique<platform_3ds::Display3ds>();
    if(!display->open()) return 1;
    render::Framebuffer frame(400,240);
    render::Framebuffer overlay(400,240);
    overlay.begin_write_coverage();
    for(unsigned y=10;y<230;++y) for(unsigned x=10;x<390;++x)
        if((x%29)<3 || (y%31)<3) overlay.set(x,y,248);
    overlay.end_write_coverage();
    render::RasterCommands commands; commands.reset(400,240);
    render::RasterCommand rect{};
    rect.left=50;rect.top=60;rect.right=350;rect.bottom=180;rect.even=100;rect.odd=104;rect.dither=true;
    commands.commands.push_back(rect);
    platform_3ds::native_engine::Scene scene;
    if(!scene.compile(commands) || !display->open_native_engine()) return 1;
    auto ppu=std::make_unique<simulation::SnesPpuState>();
    ppu->background_mode=2; ppu->main_screen=2; ppu->bg2_screen_base=0x1000;
    for(unsigned y=0;y<8;++y) ppu->vram[y*2]=255;
    render::Palette256 palette{};
    for(unsigned i=0;i<256;++i) palette[i]={std::uint8_t(i),std::uint8_t(i^0x55),std::uint8_t(i^0xaa),255};
    for(unsigned y=0;y<240;++y) for(unsigned x=0;x<400;++x) frame.set(x,y,(x/8+3*(y/8))&255);
    FILE* report=std::fopen("sdmc:/display-gpu-results.json","wb");
    if(!report)return 1;
    std::fprintf(report,"[");
    unsigned failed=0;
    for(unsigned test=0;test<16;++test) {
        const bool wide=(test&1U)==0;
        display->set_wide(wide);
        // Wireframe rendering supports dither scales 1..4. Exercise the
        // non-power-of-two scale too, including the UV boundary at row zero.
        if(test>=8 && test<14) {
            rect.left=0;rect.top=0;rect.right=400;rect.bottom=240;
            rect.reserved0=2+(test-8)/2;
            commands.commands[0]=rect;
            if(!scene.compile(commands))return 1;
        }
        if(test==14) display->use_cpu_presentation();
        bool drawn=test>=14 ? display->present(frame,palette,false,nullptr,true)
            : test>=8 ? display->present_native(frame,overlay,palette,scene)
            : test>=4 ? display->present_mode2(*ppu,1,0,0,overlay,palette,false,nullptr,test>=6?&scene:nullptr)
            : test>=2 ? display->present_native(frame,overlay,palette,scene)
            : display->present(frame,palette,false,nullptr,true);
        if(drawn && display->gpu_active()) {
            if(display->acquire_gpu_frame(1000)) C3D_FrameEnd(GX_CMDLIST_FLUSH);
            else drawn=false;
        }
        const auto* actual=reinterpret_cast<const std::uint16_t*>(display->top_framebuffer());
        if(actual) GSPGPU_InvalidateDataCache(actual,400U*240U*2U);
        const auto view=platform_3ds::top_viewport(400,240,wide);
        unsigned differences=0;
        for(unsigned y=0;y<240;++y) for(unsigned x=0;x<400;++x) {
            std::uint16_t want=0;
            if(wide || (x>=40 && x<360)) {
                const unsigned sx=view.sample_x(x),sy=view.sample_y(y);
                auto index=test>=4 && test<8?1:frame.get(sx,sy);
                if((test==2 || test==3 || test==6 || test==7 || (test>=8 && test<14))
                    && sx>=rect.left && sx<rect.right && sy>=rect.top && sy<rect.bottom)
                    if(auto sample=platform_3ds::native_engine::Scene::sample(commands,rect,sx,sy)) index=*sample;
                if(test>=2 && test<14 && overlay.get(sx,sy)) index=overlay.get(sx,sy);
                const auto c=palette[index];
                want=((c.r>>3)<<11)|((c.g>>2)<<5)|(c.b>>3);
            }
            differences+=!actual || actual[x*240+239-y]!=want;
        }
        failed+=!drawn || differences!=0;
        std::fprintf(report,"%s{\"case\":%u,\"wide\":%s,\"drawn\":%s,\"different_pixels\":%u}",
            test?",":"",test,wide?"true":"false",drawn?"true":"false",differences);
        std::fflush(report);
        if(actual) {
            char path[100];std::snprintf(path,sizeof(path),"sdmc:/display-gpu-%u.rgb565",test);
            if(auto* file=std::fopen(path,"wb")){std::fwrite(actual,400U*240U*2U,1,file);std::fclose(file);}
        }
    }
    std::fprintf(report,"]\n");std::fclose(report);
    while(aptMainLoop()) { hidScanInput(); if(hidKeysDown()&KEY_START) break; gspWaitForVBlank(); }
    display.reset();gfxExit();return failed?1:0;
}
