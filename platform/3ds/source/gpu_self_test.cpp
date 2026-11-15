#include "starfox/render/background_renderer.hpp"
#include <algorithm>
#include "gpu_self_test.hpp"
#include "display_3ds.hpp"
#include "mode1_layer_gpu_test.hpp"
#include "line_batch_gpu_test.hpp"
#include "top_viewport.hpp"
#include "screen_transfer.hpp"
#include "diagnostic_sha256.hpp"
#include "test_build_label.hpp"
#include <cstdio>
#include <memory>

namespace starfox::platform_3ds {
void write_game_pixel_comparison(const std::string& folder,
    const render::Framebuffer& reference,const render::Palette256& palette,
    std::span<const std::uint8_t> actual,bool wide,const char* backend,double alpha) {
    if (actual.size()!=400U*240U*2U) return;
    std::array<std::uint16_t,256> colours{};
    for (unsigned i=0;i<256;++i) {
        const auto c=palette[i];colours[i]=((c.r>>3)<<11)|((c.g>>2)<<5)|(c.b>>3);
    }
    std::vector<std::uint16_t> expected(400U*240U);
    transfer_indexed_screen(reference.pixels().data(),reference.stored_width(),reference.draw_scale(),
        top_viewport(reference.width(),reference.height(),wide),colours.data(),expected.data());
    unsigned different=0,small_colour_only=0,core_differences=0,bottom_differences=0;
    int first_x=-1,first_y=-1,left=400,top=240,right=-1,bottom=-1;
    std::array<unsigned,240> rows{};
    std::vector<unsigned char> diff_image(400U*240U);
    unsigned max_r=0,max_g=0,max_b=0;
    for(unsigned x=0;x<400;++x) for(unsigned y=0;y<240;++y) {
        const unsigned at=x*240+239-y;
        const auto value=std::uint16_t(actual[at*2])|(std::uint16_t(actual[at*2+1])<<8);
        if (value==expected[at]) continue;
        if(!different){first_x=x;first_y=y;} ++different;
        left=std::min(left,int(x));right=std::max(right,int(x));top=std::min(top,int(y));bottom=std::max(bottom,int(y));
        ++rows[y];diff_image[y*400+x]=255;
        core_differences+=wide?(x>=72&&x<328&&y>=8&&y<232):(x>=40&&x<360);
        bottom_differences+=y>=232;
        const unsigned dr=std::abs(int(value>>11)-int(expected[at]>>11));
        const unsigned dg=std::abs(int((value>>5)&63)-int((expected[at]>>5)&63));
        const unsigned db=std::abs(int(value&31)-int(expected[at]&31));
        small_colour_only+=dr<=1&&dg<=1&&db<=1;
        max_r=std::max(max_r,dr);max_g=std::max(max_g,dg);max_b=std::max(max_b,db);
    }
    bool saved=false;
    if(auto* f=std::fopen((folder+"/reference.rgb565").c_str(),"wb")) {
        saved=std::fwrite(expected.data(),sizeof(std::uint16_t),expected.size(),f)==expected.size();
        saved=std::fclose(f)==0 && saved;
    }
    if(auto* f=std::fopen((folder+"/pixel-comparison.json").c_str(),"wb")) {
        std::fprintf(f,"{\"schema\":2,\"status\":\"compared\",\"oracle\":\"local_cpu_renderer_not_independent_snes\",\"backend\":\"%s\",\"interpolation_alpha\":%.9f,"
            "\"wide\":%s,\"compared_pixels\":96000,\"different_pixels\":%u,"
            "\"first_x\":%d,\"first_y\":%d,\"reference_saved\":%s,"
            "\"reference_layout\":\"little_endian_rgb565_column_major_y_reversed_400x240\",",
            backend,alpha,wide?"true":"false",different,first_x,first_y,saved?"true":"false");
        std::fprintf(f,"\"core_different_pixels\":%u,\"bottom8_different_pixels\":%u,\"colour_within_one_step\":%u,"
            "\"max_channel_steps\":[%u,%u,%u],\"bbox\":[%d,%d,%d,%d],\"different_pixels_by_row\":[",
            core_differences,bottom_differences,small_colour_only,max_r,max_g,max_b,
            different?left:-1,different?top:-1,right,bottom);
        for(unsigned y=0;y<240;++y)std::fprintf(f,"%s%u",y?",":"",rows[y]);
        std::fputs("]}\n",f);
        std::fclose(f);
    }
    if(auto* f=std::fopen((folder+"/pixel-diff.pgm").c_str(),"wb")) {
        std::fputs("P5\n400 240\n255\n",f);std::fwrite(diff_image.data(),1,diff_image.size(),f);std::fclose(f);
    }
    append_diagnostic_hashes(folder,{"reference.rgb565","pixel-comparison.json","pixel-diff.pgm"});
}
void append_diagnostic_hashes(const std::string& folder,std::initializer_list<const char*> names) {
    for(const auto* name:names) {
        auto* input=std::fopen((folder+"/"+name).c_str(),"rb");
        if(!input) continue;
        DiagnosticSha256 sha;std::array<std::uint8_t,4096> bytes;
        while(const auto n=std::fread(bytes.data(),1,bytes.size(),input)) sha.update({bytes.data(),n});
        const bool ok=!std::ferror(input);std::fclose(input);
        if(ok) if(auto* hashes=std::fopen((folder+"/SHA256SUMS.txt").c_str(),"ab")) {
            for(auto byte:sha.finish()) std::fprintf(hashes,"%02x",byte);
            std::fprintf(hashes,"  %s\n",name);std::fclose(hashes);
        }
    }
}
void write_pixel_comparison_skipped(const std::string& folder,const char* reason) {
    if(auto* f=std::fopen((folder+"/pixel-comparison.json").c_str(),"wb")) {
        std::fprintf(f,"{\"schema\":2,\"status\":\"skipped\",\"reason\":\"%s\"}\n",reason);
        std::fclose(f);
    }
    append_diagnostic_hashes(folder,{"pixel-comparison.json"});
}
void write_presentation_snapshot(const std::string& folder,const simulation::SnesPpuState& ppu,
    int scroll_x,int scroll_y,double alpha,const render::RasterCommands& commands,bool native) {
    const auto save=[&](const char* name,const void* data,std::size_t size) {
        auto* f=std::fopen((folder+"/"+name).c_str(),"wb");if(!f)return false;
        const bool ok=std::fwrite(data,1,size,f)==size;return std::fclose(f)==0 && ok;
    };
    const bool ppu_saved=save("presentation-ppu.bin",&ppu,sizeof(ppu));
    bool commands_saved=false,texels_saved=false;
    if(native) {
        commands_saved=save("native-commands.bin",commands.commands.data(),commands.commands.size()*sizeof(render::RasterCommand));
        texels_saved=save("native-texels.bin",commands.texels.data(),commands.texels.size());
    }
    if(auto* f=std::fopen((folder+"/presentation-state.json").c_str(),"wb")) {
        std::fprintf(f,"{\"schema\":2,\"build\":\"%.*s\",\"ppu_layout\":\"native_SnesPpuState_little_endian\","
            "\"ppu_bytes\":%u,\"ppu_saved\":%s,\"scroll_x\":%d,\"scroll_y\":%d,\"alpha\":%.9f,"
            "\"native\":%s,\"width\":%u,\"height\":%u,\"command_stride\":%u,\"command_count\":%u,"
            "\"commands_saved\":%s,\"texels_saved\":%s}\n",
            static_cast<int>(test_build_label.size()),test_build_label.data(),
            unsigned(sizeof(ppu)),ppu_saved?"true":"false",scroll_x,scroll_y,alpha,native?"true":"false",
            unsigned(commands.width()),unsigned(commands.height()),unsigned(sizeof(render::RasterCommand)),
            native?unsigned(commands.commands.size()):0,commands_saved?"true":"false",texels_saved?"true":"false");
        std::fclose(f);
    }
    append_diagnostic_hashes(folder,{"presentation-ppu.bin","presentation-state.json","native-commands.bin","native-texels.bin"});
}

void run_gpu_self_test(Display3ds& display,const std::string& folder,bool restore_wide) {
    const auto close_file=[](FILE* file) { if (file) std::fclose(file); };
    std::unique_ptr<FILE,decltype(close_file)> file(
        std::fopen((folder+"/gpu-pixels.jsonl").c_str(),"wb"),close_file);
    if (!file) return;
    auto* report=file.get();
    const auto event=[&](unsigned test,const char* stage) {
        std::fprintf(report,"{\"case\":%u,\"stage\":\"%s\",\"gpu_active\":%s}\n",
            test,stage,display.gpu_active()?"true":"false");
        std::fflush(report);
    };
    event(0,"begin");
    if (!display.gpu_active() || !display.native_engine_ready()) {
        event(0,"unavailable"); return;
    }
    render::Framebuffer frame(400,240),overlay(400,240);
    overlay.begin_write_coverage();
    for (unsigned y=10;y<230;++y) for (unsigned x=10;x<390;++x)
        if (x%29<3 || y%31<3) overlay.set(x,y,248);
    overlay.end_write_coverage();
    for (unsigned y=0;y<240;++y) for (unsigned x=0;x<400;++x)
        frame.set(x,y,(x/8+3*(y/8))&255);
    auto ppu=std::make_unique<simulation::SnesPpuState>();
    ppu->background_mode=2; ppu->main_screen=2; ppu->bg2_screen_base=0x1000;
    ppu->bg2_character_base=0; // The fixture below writes character zero at VRAM zero.
    for (unsigned y=0;y<8;++y) ppu->vram[y*2]=255;
    render::Palette256 palette{};
    // Real cartridge colours expand five-bit CGRAM channels. This also
    // exercises the RGB5A1 overlay without demanding nonexistent precision.
    const auto expand=[](unsigned v) { v&=31; return std::uint8_t((v<<3)|(v>>2)); };
    for (unsigned i=0;i<256;++i) palette[i]={expand(i),expand(i^0x15),expand(i^0x0a),255};
    render::RasterCommands commands; commands.reset(400,240);
    native_engine::Scene scene;
    unsigned completed=0,failed=0;
    constexpr unsigned cases=60;
    render::Framebuffer bg_expected(400,240);
    for (unsigned test=0;test<cases;++test) {
        if (!aptMainLoop()) { event(test,"apt_exit"); break; }
        const bool wide=(test&1)==0;
        char label[40];std::snprintf(label,sizeof(label),"GPU %u/%u",test+1,cases);
        (void)display.show_test_status(label,"COMPARANDO PIXELES",test*100/cases);
        event(test,"fence_before");
        if (!display.wait_gpu_idle(500)) {
            event(test,"timeout_before"); display.use_cpu_presentation(); break;
        }
        display.set_wide(wide);
        render::RasterCommand rect{};
        rect.left=50; rect.top=60; rect.right=350; rect.bottom=180;
        rect.even=100; rect.odd=104; rect.dither=true;
        if (test>=8) {
            rect.left=0; rect.top=0; rect.right=400; rect.bottom=240;
            rect.reserved0=2+(test-8)/2%3;
        }
        // Repeat the same material keys after changing their colours.
        if (test==14) { palette[100]={255,0,0,255}; palette[104]={0,255,255,255}; }
        if (test>=16) { rect.even=0; rect.odd=104; rect.reserved0=1; }
        commands.commands.clear();
        // Palette zero must erase older world geometry, exposing the BG.
        if (test>=18) { auto under=rect; under.even=under.odd=100; commands.commands.push_back(under); }
        commands.commands.push_back(rect);
        if (!scene.compile(commands)) { event(test,"scene_failed"); ++failed; break; }
        const bool hdma=test>=20 && test<36;
        const bool edges=test>=36;
        const bool mode2=(test>=4 && test<8)||hdma;
        const bool world=test!=58 && (hdma?(test&2U)!=0U:test==2||test==3||test>=6);
        // The last eight cases exercise retained Mode 1 textures: repeated
        // frames, opaque palette-zero writes, a changed palette/background,
        // and a different presenter overwriting the shared texture storage.
        if (test==54) {
            overlay.begin_write_coverage();
            for (unsigned y=10;y<230;++y) for (unsigned x=10;x<390;++x)
                if (x%29<3 || y%31<3) overlay.set(x,y,248);
            for (unsigned y=90;y<120;++y) for (unsigned x=150;x<220;++x)
                overlay.set(x,y,0);
            overlay.end_write_coverage();
        }
        if (test==56) palette[248]={expand(4),expand(29),expand(9),255};
        if (test==57)
            for(unsigned y=180;y<220;++y) for(unsigned x=20;x<60;++x) frame.set(x,y,61);
        if (hdma) {
            *ppu={};ppu->background_mode=2;ppu->main_screen=2;
            ppu->bg2_character_base=0x1000;ppu->bg2_screen_base=0x6000;
            ppu->bg2_vertical_offsets_enabled=true;ppu->bg2_horizontal_offsets_enabled=true;
            const auto word=[&](unsigned address,unsigned value){ppu->vram[address*2]=value;ppu->vram[address*2+1]=value>>8;};
            for(unsigned ch=1;ch<=16;++ch)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
                const unsigned ink=1+(x+y+ch)%15;
                for(unsigned plane=0;plane<4;++plane)if(ink&(1U<<plane))
                    ppu->vram[0x2000+ch*32+y*2+(plane&1)+(plane/2)*16]|=0x80U>>x;
            }
            // Fill all four map pages, so affine cases exercise textured
            // pixels at every Y/phase rather than mostly transparent space.
            for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
                const unsigned entry=((x/32)+(y/32)*2)*1024+(y%32)*32+(x%32);
                word(0x6000+entry,1+(x+3*y)%16+(((x/4+y/4)%8)<<10));
            }
            const int slope=int((test-20)/4)-1;
            for(unsigned x=0;x<32;++x)word(0x2fa0+x,0x4000|((300+slope*int(x))&8191));
            for(unsigned y=0;y<224;++y)ppu->bg2_horizontal_offsets[y]=int(y*3%71)+int(test&7U);
            bg_expected.clear(0);
            render::BackgroundRenderer{}.draw_bg2(*ppu,0,0,bg_expected,render::TilePriorityPass::all,72,true,true,false,0,{},8);
            rect.left=120;rect.right=280;rect.top=90;rect.bottom=150;rect.even=100;rect.odd=104;rect.reserved0=1;
        }
        if(edges) {
            rect.left=10;rect.right=390;rect.top=10;rect.bottom=225;
            rect.u=25*256+int(test*17%256);rect.v=270*256+int(test*39%256);
            rect.du=int(test%5)*73-110;rect.dv=int(test%7)*29-80;
            rect.reserved1=8;rect.reserved0=1;rect.even=std::uint8_t(32+test);rect.odd=std::uint8_t(96+test);
        }
        if(hdma||edges) {
            commands.commands.clear();commands.commands.push_back(rect);
            if(!scene.compile(commands)) {event(test,"scene_failed");++failed;break;}
        }
        event(test,"submit");
        const auto begin=svcGetSystemTick();
        const bool drawn=mode2
            ? display.present_mode2(*ppu,0xffffffffffffffffULL-test,0,0,overlay,palette,
                false,nullptr,world?&scene:nullptr)
            : world ? display.present_native(frame,overlay,palette,scene,
                test>=52 ? (test>=57?2U:1U) : 0U,
                test>=52 ? (test>=54?2U:1U) : 0U)
            : display.present(frame,palette,false,nullptr,true);
        event(test,"fence_after");
        if (!display.wait_gpu_idle(500)) {
            event(test,"timeout_after"); display.use_cpu_presentation(); ++failed; break;
        }
        const auto gpu_us=(svcGetSystemTick()-begin)*1'000'000ULL/SYSCLOCK_ARM11;
        const auto* actual=reinterpret_cast<const std::uint16_t*>(display.top_framebuffer());
        const bool readable=actual && R_SUCCEEDED(GSPGPU_InvalidateDataCache(actual,400U*240U*2U));
        const auto view=top_viewport(400,240,wide);
        unsigned differences=0;
        std::vector<std::uint16_t> expected(400U*240U);
        for (unsigned y=0;y<240;++y) for (unsigned x=0;x<400;++x) {
            std::uint16_t want=0;
            if (wide || (x>=40 && x<360)) {
                const unsigned sx=view.sample_x(x),sy=view.sample_y(y);
                unsigned index=hdma?bg_expected.get(sx,sy):mode2?1:frame.get(sx,sy);
                if (world && sx>=unsigned(rect.left) && sx<unsigned(rect.right)
                    && sy>=unsigned(rect.top) && sy<unsigned(rect.bottom)) {
                    const auto colour=native_engine::Scene::sample(commands,rect,sx,sy);
                    bool inside=true;
                    if(edges) {
                        const int dy=int(sy)-rect.top;
                        const int left_edge=(rect.u+rect.du*dy)>>8;
                        const int right_edge=(rect.v+rect.dv*dy)>>8;
                        inside=int(sx)>=left_edge && int(sx)<=right_edge;
                    }
                    if (inside && colour && *colour) index=*colour;
                }
                if ((mode2 || world) && overlay.write_coverage()[sy*400U+sx]) index=overlay.get(sx,sy);
                const auto c=palette[index]; want=((c.r>>3)<<11)|((c.g>>2)<<5)|(c.b>>3);
            }
            expected[x*240+239-y]=want;
            differences+=!readable || actual[x*240+239-y]!=want;
        }
        const bool reuse_expected=test==53 || test==55;
        const bool reuse_correct=test<52 || test==58
            || display.last_frame_timing().reuse_hit==reuse_expected;
        ++completed; failed+=!drawn || differences!=0 || !reuse_correct;
        std::fprintf(report,"{\"case\":%u,\"stage\":\"complete\",\"wide\":%s,"
            "\"drawn\":%s,\"different_pixels\":%u,\"submit_and_fence_us\":%llu,\"fixture\":\"%s\",\"reuse_correct\":%s}\n",
            test,wide?"true":"false",drawn?"true":"false",differences,
            static_cast<unsigned long long>(gpu_us),test>=52?"retained_mode1_layers":hdma?"mode2_affine_hdma":edges?"subpixel_edges":mode2?"mode2_flat":"upload_and_dither",
            reuse_correct?"true":"false");
        std::fflush(report);
        if (readable && differences) {
            char name[48];std::snprintf(name,sizeof(name),"/gpu-case-%02u.rgb565",test);
            char expected_name[64];std::snprintf(expected_name,sizeof(expected_name),"/gpu-case-%02u-expected.rgb565",test);
            if(auto* f=std::fopen((folder+expected_name).c_str(),"wb")){std::fwrite(expected.data(),2,expected.size(),f);std::fclose(f);}
            if (auto* capture=std::fopen((folder+name).c_str(),"wb")) {
                std::fwrite(actual,400U*240U*2U,1,capture); std::fclose(capture);
            }
        }
    }
    if (completed==cases && display.gpu_active())
        run_mode1_layer_gpu_tests(display,report,folder,completed,failed);
    if (completed==cases+16U && display.gpu_active())
        run_line_batch_gpu_tests(display,report,completed,failed);
    display.set_wide(restore_wide);
    std::fprintf(report,"{\"summary\":true,\"completed\":%u,\"failed\":%u,"
        "\"expected\":%u,\"gpu_active\":%s}\n",completed,failed,cases+24U,
        display.gpu_active()?"true":"false");
}
}
