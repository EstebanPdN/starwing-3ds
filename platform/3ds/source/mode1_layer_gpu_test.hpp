#pragma once
#include "mode1_layer_plan.hpp"
#include "display_3ds.hpp"
#include <cstdio>
#include <memory>
#include <string>
// Sixteen on-console GPU cases for retained Mode 1 layers, run by
// run_gpu_self_test (gpu_self_test.cpp) after its own 60 cases. 3DS only.

namespace starfox::platform_3ds {
// Differential device check: the established two-screen-texture presenter
// and the retained atlas presenter must produce identical RGB565 output.
// The separate host plan test checks the integer scroll/HDMA oracle.
// Appends one JSON line per case (numbered 60-75) to `report`, adds to
// `completed` and `failed`, and saves both pictures of a mismatch in `folder`.
inline void run_mode1_layer_gpu_tests(Display3ds& display,FILE* report,
    const std::string& folder,unsigned& completed,unsigned& failed) {
    auto p=std::make_unique<simulation::SnesPpuState>();
    auto plan=std::make_unique<Mode1LayerPlan>();
    render::Framebuffer panorama_low(1536,512),panorama_high(1536,512);
    render::Framebuffer tiles_low(512,512),tiles_high(512,512);
    for (unsigned y=0;y<512U;++y) for(unsigned x=0;x<1536U;++x) {
        const unsigned c=(x*13U+y*7U)%47U;
        if (c%5U) ((x/17U+y/11U)&1U?panorama_low:panorama_high).set(x,y,std::uint8_t(c+1U));
    }
    for (unsigned y=0;y<512U;++y) for(unsigned x=0;x<512U;++x) {
        const unsigned c=(x*3U+y*17U)%31U;
        if (c%7U) ((x/13U+y/9U)&1U?tiles_low:tiles_high).set(x,y,std::uint8_t(c+64U));
    }
    render::Palette256 palette{};
    // Include colours between SNES 5-bit steps, as brightness fades do. This
    // catches accidentally quantizing the background as RGB5A1 instead of 565.
    for (unsigned i=0;i<256U;++i) palette[i]={std::uint8_t(i*29U),std::uint8_t(i*19U),std::uint8_t(i*43U),255};
    p->background_mode=1;p->main_screen=6;p->bg2_screen_size=3;p->bg3_screen_size=3;
    for (unsigned i=0;i<256U;++i) p->cgram[i]=std::uint16_t(i*137U);
    render::RasterCommands commands;commands.reset(400,240);
    render::RasterCommand world{};world.left=40;world.top=30;world.right=370;world.bottom=205;
    world.even=108;world.odd=120;world.dither=true;commands.commands.push_back(world);
    world.left=100;world.right=190;world.top=80;world.bottom=125;world.even=0;world.odd=0;
    commands.commands.push_back(world);
    native_engine::Scene scene;
    if (!scene.compile(commands)) { ++failed; return; }
    constexpr unsigned cases=16U;
    for(unsigned test=0;test<cases;++test) {
        if (!aptMainLoop() || !display.wait_gpu_idle(500)) { ++failed;break; }
        display.set_wide((test&1U)==0U);
        p->bg3_high_priority=(test&2U)!=0U;
        p->bg3_scroll_x=std::array<std::int16_t,4>{-512,-1,0,511}[test%4U];
        p->bg3_scroll_y=std::int16_t(491U+test*19U);
        p->bg2_horizontal_offsets_enabled=test>=4U;
        p->bg2_scanline_scroll_enabled=test>=8U;
        p->tunnel_scene=test>=12U;
        for (unsigned row=0;row<224U;++row) {
            p->bg2_horizontal_offsets[row]=std::int16_t(row*19U+test*71U);
            p->bg2_scanline_scroll_y[row]=std::int16_t(row*13U+test*37U);
        }
        if (test==10U) { palette[20].g^=15U;palette[75].g^=27U; }
        if (test==5U) for(unsigned y=0;y<8U;++y)for(unsigned x=0;x<8U;++x) {
            panorama_low.set(576U+x,74U+y,25U);
            panorama_high.set(576U+x,74U+y,26U);
        }
        if (test==12U) { tiles_low.clear();tiles_high.clear(); }
        if (test==14U) { tiles_high.set(0,223,66);tiles_low.set(511,35,81); }
        const unsigned revision=test>=14U?3U:test>=12U?2U:1U;
        const bool planned=plan->build(*p,int(test*89)-150,int(test*37),panorama_low,panorama_high,
            test>=5U?2U:1U,tiles_low,tiles_high,revision);
        render::Framebuffer background(400,240),foreground(400,240),hud(400,240);
        for(unsigned layer:{0U,1U,2U})if(!plan->above_world(layer))plan->replay_layer(layer,background);
        foreground.begin_write_coverage();
        plan->replay_layer(3,foreground);
        if(plan->high_priority())plan->replay_layer(1,foreground);
        hud.begin_write_coverage();
        for(unsigned y=7;y<48U;++y)for(unsigned x=5;x<395U;++x) {
            if ((x+y)%7U) continue;
            const auto colour=std::uint8_t(x%19U?123U:0U);
            hud.set(x,y,colour);foreground.set(x,y,colour);
        }
        hud.end_write_coverage();foreground.end_write_coverage();
        const bool before=planned && display.present_native(background,foreground,palette,scene);
        bool readable=before && display.wait_gpu_idle(500);
        std::vector<std::uint16_t> expected(96000U);
        const auto* fb=display.top_framebuffer();
        readable=readable && fb && R_SUCCEEDED(GSPGPU_InvalidateDataCache(fb,192000U));
        if (readable) std::memcpy(expected.data(),fb,192000U);
        const bool after=readable && display.present_mode1_layers(*plan,hud,palette,scene,test+1U);
        readable=after && display.wait_gpu_idle(500);
        fb=display.top_framebuffer();
        readable=readable && fb && R_SUCCEEDED(GSPGPU_InvalidateDataCache(fb,192000U));
        unsigned differences{};
        if (readable) for(unsigned i=0;i<96000U;++i) {
            const auto got=std::uint16_t(fb[i*2U])|(std::uint16_t(fb[i*2U+1U])<<8U);
            differences+=got!=expected[i];
        } else differences=96000U;
        const auto uploads=display.last_frame_timing().mode1_layer_upload_bytes;
        const auto converted=display.last_frame_timing().mode1_layer_converted_bytes;
        const auto quads=display.last_frame_timing().mode1_layer_quads;
        const bool partial_correct=test!=5U || (converted>0U && converted<=4096U);
        bool reused=false;
        if (readable && !differences && display.present_mode1_layers(*plan,hud,palette,scene,test+1U)
            && display.wait_gpu_idle(500))
            reused=display.last_frame_timing().reuse_hit
                && display.last_frame_timing().mode1_layer_upload_bytes==0U;
        ++completed;failed+=!readable || differences!=0U || !reused || !partial_correct;
        std::fprintf(report,"{\"case\":%u,\"fixture\":\"mode1_retained_atlas\",\"drawn\":%s,"
            "\"different_pixels\":%u,\"uploaded_bytes\":%u,\"converted_bytes\":%u,\"quads\":%u,\"reuse_correct\":%s,\"partial_correct\":%s}\n",
            test+60U,readable?"true":"false",differences,unsigned(uploads),unsigned(converted),unsigned(quads),
            reused?"true":"false",partial_correct?"true":"false");
        std::fflush(report);
        if (differences && readable) {
            const auto prefix=folder+"/gpu-layer-case-"+std::to_string(test);
            if(auto* f=std::fopen((prefix+"-expected.rgb565").c_str(),"wb")) {
                std::fwrite(expected.data(),2,expected.size(),f);std::fclose(f);
            }
            if(auto* f=std::fopen((prefix+"-actual.rgb565").c_str(),"wb")) {
                std::fwrite(fb,2,96000U,f);std::fclose(f);
            }
        }
    }
}
} // namespace starfox::platform_3ds
