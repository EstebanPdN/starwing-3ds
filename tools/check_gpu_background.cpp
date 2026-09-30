#include "starfox/render/gpu_background.hpp"
#include "starfox/render/gpu_scene.hpp"
#include "starfox/render/gpu_composite.hpp"
#include "starfox/render/terrain_profile.hpp"
#include <SDL3/SDL.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <fstream>
using namespace starfox::render;
namespace {
void require(bool value,const char* message) {if(!value) throw std::runtime_error(std::string(message)+": "+SDL_GetError());}
std::vector<std::uint32_t> read_pixels(SDL_GPUDevice*,SDL_GPUCommandBuffer*,const GpuRasterOutput&);
std::vector<std::uint32_t> run(SDL_GPUDevice* device,GpuBackground& gpu,
    const starfox::simulation::SnesPpuState& ppu,unsigned w,unsigned h,unsigned scale,const GpuBackgroundSettings& settings) {
    auto* cmd=SDL_AcquireGPUCommandBuffer(device);require(cmd,"command");
    const auto result=gpu.enqueue(device,cmd,ppu,w*scale,h*scale,scale,settings);
    if(!result.pixels) {SDL_CancelGPUCommandBuffer(cmd);throw std::runtime_error(gpu.status());}
    return read_pixels(device,cmd,result);
}
std::vector<std::uint32_t> read_pixels(SDL_GPUDevice* device,SDL_GPUCommandBuffer* cmd,const GpuRasterOutput& result) {
    const unsigned bytes=result.width*result.height*4;
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,bytes,0};
    auto* download=SDL_CreateGPUTransferBuffer(device,&info);require(download,"download");
    auto* copy=SDL_BeginGPUCopyPass(cmd);require(copy,"copy");
    SDL_GPUBufferRegion from{static_cast<SDL_GPUBuffer*>(result.pixels),0,bytes};SDL_GPUTransferBufferLocation to{download,0};
    SDL_DownloadFromGPUBuffer(copy,&from,&to);SDL_EndGPUCopyPass(copy);
    auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);require(fence,"submit");
    require(SDL_WaitForGPUFences(device,true,&fence,1),"wait");SDL_ReleaseGPUFence(device,fence);
    auto* mapped=SDL_MapGPUTransferBuffer(device,download,false);require(mapped,"map");
    std::vector<std::uint32_t> output(bytes/4);std::memcpy(output.data(),mapped,bytes);
    SDL_UnmapGPUTransferBuffer(device,download);SDL_ReleaseGPUTransferBuffer(device,download);return output;
}
}
int main(int argc,char** argv) {
    SDL_GPUDevice* device{};
    try {
        require(SDL_Init(SDL_INIT_VIDEO),"SDL init");
        auto props=SDL_CreateProperties();
        SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,true);
        SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN,true);
        SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN,true);
        SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,false);
        device=SDL_CreateGPUDeviceWithProperties(props);SDL_DestroyProperties(props);require(device,"GPU device");
        {
        GpuBackground gpu;BackgroundRenderer reference;
        auto ppu=std::make_unique<starfox::simulation::SnesPpuState>();
        if(argc>1) {
            std::ifstream asset(argv[1],std::ios::binary);std::array<char,8192> bytes{};
            require(bool(asset.read(bytes.data(),bytes.size())) && asset.peek()==EOF,"authored terrain fixture");
            ppu->bg2_screen_base=0x7000;ppu->bg2_screen_size=3;ppu->background_mode=2;ppu->bg2_tile_size_16=false;
            std::memcpy(ppu->vram.data()+0xe000,bytes.data(),bytes.size());
            require(authored_terrain_rows(*ppu)==std::array<std::uint32_t,2>{360,512},"ST-P terrain definition");
            ppu->vram[0xe000]^=1;require(authored_terrain_rows(*ppu)==std::array<std::uint32_t,2>{},"changed tilemap accepted");
            ppu->vram[0xe000]^=1;ppu->tunnel_scene=true;
            require(authored_terrain_rows(*ppu)==std::array<std::uint32_t,2>{},"tunnel terrain definition");
            ppu->tunnel_scene=false;ppu->bg2_tile_size_16=true;
            require(authored_terrain_rows(*ppu)==std::array<std::uint32_t,2>{},"wrong tile size accepted");
            ppu->bg2_tile_size_16=false;
            for(unsigned i=0;i<8192;i+=2) {
                const unsigned word=ppu->vram[0xe000+i]|(unsigned(ppu->vram[0xe001+i])<<8);
                const unsigned relocated=(word&0xfc00u)|((word+192)&1023u);
                ppu->vram[0xe000+i]=std::uint8_t(relocated);ppu->vram[0xe001+i]=std::uint8_t(relocated>>8);
            }
            require(authored_terrain_rows(*ppu)==std::array<std::uint32_t,2>{360,512},"relocated ST-P terrain definition");
        }
        unsigned cases=0;std::uint64_t samples=0,ink=0;
        for(unsigned layer:{1U,3U}) for(unsigned mode:{1U,2U,3U}) for(unsigned size=0;size<4;++size)
        for(unsigned scale:{1U,2U,3U,4U}) for(unsigned priority=0;priority<3;++priority) {
            const unsigned w=priority==0?256:priority==1?400:796,h=64;
            std::uint32_t random=0x31f567abU+cases;
            for(auto& v:ppu->vram) {random=random*1664525U+1013904223U;v=std::uint8_t(random>>24);}
            for(unsigned i=0;i<256;++i) ppu->cgram[i]=i%5?std::uint16_t(i*19):0;
            ppu->background_mode=cases%17?mode:0;ppu->main_screen=cases%13?5:0;
            ppu->bg1_screen_size=ppu->bg3_screen_size=size;
            ppu->bg1_tile_size_16=ppu->bg3_tile_size_16=cases%2;
            ppu->bg1_screen_base=ppu->bg3_screen_base=cases%2?0x7fc0:0x4000;
            ppu->bg1_character_base=ppu->bg3_character_base=cases%2?0xfff8:0x1200;
            ppu->bg1_scroll_x=ppu->bg3_scroll_x=cases%2?-32768:32767;
            ppu->bg1_scroll_y=ppu->bg3_scroll_y=std::int16_t(cases*31-1000);
            ppu->mosaic=std::array<unsigned,4>{0,0xf5,0x21,0x74}[cases%4];
            GpuBackgroundSettings s;s.layer=layer;s.priority=TilePriorityPass(priority);
            s.horizontal_origin=std::array<int,3>{-17,72,300}[cases%3];s.extend_horizontal=cases%2;
            s.horizontal_inset=std::array<unsigned,3>{0,16,129}[cases%3];s.transparent_cgram_black=cases%2;
            Framebuffer expected(w,h,scale);expected.enable_layer_tags(true);expected.begin_write_coverage();
            {ScopedLayer tag(expected,s.tag);
            if(layer==1) reference.draw_bg1(*ppu,expected,s.priority,s.horizontal_origin,s.extend_horizontal,s.horizontal_inset,s.transparent_cgram_black);
            else reference.draw_bg3(*ppu,expected,s.priority,s.horizontal_origin,s.extend_horizontal);}
            const auto actual=run(device,gpu,*ppu,w,h,scale,s);
            for(std::size_t i=0;i<actual.size();++i) {
                const std::uint32_t wanted=expected.write_coverage()[i]
                    ?std::uint32_t(expected.pixels()[i])|(unsigned(s.tag)<<8)|0x04000000U:0;
                if(actual[i]!=wanted) {
                    std::cerr<<"BG mismatch case="<<cases<<" layer="<<layer<<" mode="<<mode<<" size="<<size<<" scale="<<scale<<" pixel="<<i<<" got="<<actual[i]<<" expected="<<wanted<<'\n';
                    throw std::runtime_error("GPU tile parity");
                }
                ink+=wanted!=0;
            }
            samples+=actual.size();++cases;
            if(cases%37==0) gpu.release_device();
        }
        for(unsigned variant=0;variant<144;++variant) {
            const unsigned scale=1+variant%4,w=std::array<unsigned,3>{256,400,796}[(variant/4)%3];
            const unsigned h=std::array<unsigned,3>{192,224,256}[(variant/12)%3];
            std::uint32_t random=0x793481abU+variant;
            for(auto& v:ppu->vram) {random=random*1664525U+1013904223U;v=std::uint8_t(random>>24);}
            for(unsigned i=0;i<256;++i) ppu->cgram[i]=variant%2?std::uint16_t(1+i*23):std::uint16_t(i%5?i*19:0);
            ppu->background_mode=variant%5?2:1;ppu->main_screen=variant%19?7:0;
            ppu->bg2_screen_size=(variant/3)%4;ppu->bg2_tile_size_16=variant%2;
            ppu->bg2_screen_base=variant%2?0x7fc0:0x4000;ppu->bg2_character_base=variant%2?0xfff8:0x1200;
            ppu->bg2_scroll_x=-123;ppu->bg2_scroll_y=237;
            ppu->bg2_vertical_offsets_enabled=variant%7!=0;
            ppu->bg2_horizontal_offsets_enabled=variant%3!=0;
            ppu->bg2_scanline_scroll_enabled=variant%4!=0;ppu->tunnel_scene=variant%11==0;
            ppu->mosaic=std::array<unsigned,4>{0,0xf2,0x22,0x72}[(variant/4)%4];
            for(unsigned i=0;i<224;++i) {
                ppu->bg2_horizontal_offsets[i]=std::int16_t(int(i)*3-320);
                ppu->bg2_scanline_scroll_y[i]=std::int16_t(i<112?64:320);
            }
            for(unsigned i=0;i<32;++i) {
                const int slope=std::array<int,4>{1,-7,83,-133}[(variant/6)%4];
                const auto value=unsigned(8180+int(i)*slope/3)&8191U;
                const auto word=value|((variant%6==0 || (variant%6==1 && i!=13) || (variant%6==2 && i%3==0))?0:0x4000U);
                ppu->vram[0x5f40+i*2]=std::uint8_t(word);ppu->vram[0x5f41+i*2]=std::uint8_t(word>>8);
            }
            GpuBackgroundSettings s;s.layer=2;s.priority=TilePriorityPass(variant%3);
            s.horizontal_origin=std::array<int,3>{-17,72,270}[(variant/3)%3];s.extend_horizontal=variant%9!=0;
            s.scroll_x=int(variant)*7-500;s.scroll_y=int(variant)*11-800;
            s.wrap_horizontal=variant%8!=0;s.transparent_cgram_black=variant%2==0;
            s.single_occurrence_top_rows=variant%4==0?96:0;
            if(variant%5==0) s.unique_regions={{0,0,1024,1024,1,63,0},{0,0,1024,1024,64,127,3}};
            Framebuffer expected(w,h,scale);expected.enable_layer_tags(true);expected.begin_write_coverage();
            {ScopedLayer tag(expected,s.tag);reference.draw_bg2(*ppu,s.scroll_x,s.scroll_y,expected,s.priority,
                s.horizontal_origin,s.extend_horizontal,s.wrap_horizontal,s.transparent_cgram_black,s.single_occurrence_top_rows,s.unique_regions);}
            const auto actual=run(device,gpu,*ppu,w,h,scale,s);
            for(std::size_t i=0;i<actual.size();++i) {
                const std::uint32_t wanted=expected.write_coverage()[i]?std::uint32_t(expected.pixels()[i])|(unsigned(s.tag)<<8)|0x04000000U:0;
                if(actual[i]!=wanted) {
                    std::cerr<<"BG2 mismatch variant="<<variant<<" pixel="<<i<<" got="<<actual[i]<<" expected="<<wanted<<'\n';
                    throw std::runtime_error("GPU BG2 parity");
                }
                ink+=wanted!=0;
            }
            samples+=actual.size();++cases;
            if(variant%37==0) gpu.release_device();
        }
        require(ink>100000,"empty tile fixtures");
        {
            // Terrain follows authored source rows after scrolling, not a
            // screen-space horizon. Classification must not change colour.
            ppu->background_mode=2;ppu->main_screen=2;ppu->mosaic=0;
            ppu->bg2_vertical_offsets_enabled=false;ppu->bg2_horizontal_offsets_enabled=false;
            ppu->bg2_scanline_scroll_enabled=false;ppu->bg2_tile_size_16=false;
            ppu->bg2_screen_size=0;ppu->tunnel_scene=false;
            for(unsigned scale:{1u,2u,4u}) for(int scroll:{-12,0,17}) {
                GpuBackgroundSettings s;s.layer=2;s.scroll_y=scroll;s.extend_horizontal=false;
                const auto base=run(device,gpu,*ppu,256,64,scale,s);
                s.terrain_source_rows={16,40};
                const auto classified=run(device,gpu,*ppu,256,64,scale,s);
                for(std::size_t i=0;i<base.size();++i) {
                    const unsigned source_y=unsigned(int(i/(256*scale)/scale)+scroll)&255u;
                    const bool terrain=(base[i]&0x04000000u) && source_y>=16 && source_y<40;
                    require(classified[i]==(base[i]|(terrain?0x08000000u:0)),"terrain source-row classification");
                }
                ppu->tunnel_scene=true;
                const auto tunnel=run(device,gpu,*ppu,256,64,scale,s);
                for(auto value:tunnel) require(!(value&0x08000000u),"tunnel classified as terrain");
                ppu->tunnel_scene=false;
            }
            GpuBackgroundSettings s;s.layer=2;s.terrain_source_rows={0,256};
            const auto expected=run(device,gpu,*ppu,256,64,1,s);
            auto* command=SDL_AcquireGPUCommandBuffer(device);require(command,"terrain background command");
            const auto background_output=gpu.enqueue(device,command,*ppu,256,64,1,s);
            require(background_output.pixels && SDL_SubmitGPUCommandBuffer(command),"terrain background submit");
            RasterCommands native;native.reset(256,64);RasterCommand ink{};
            ink.left=64;ink.right=128;ink.top=0;ink.bottom=64;ink.even=ink.odd=7;native.add(ink);
            GpuRaster raster;require(raster.render_resident(device,native,false),"terrain model layer");
            Framebuffer cpu(256,64);cpu.enable_layer_tags(true);std::vector<std::uint8_t> coverage(256*64);
            for(unsigned y=0;y<64;++y) for(unsigned x=192;x<256;++x) {
                cpu.set_stored(x,y,0,PixelLayer::two_d);coverage[y*256+x]=1;
            }
            GpuComposite compositor;GpuCompositeBackground background{background_output,{}};
            LayerCompositeSettings settings;settings.clip_right=256;settings.clip_bottom=64;
            std::array<Rgba8,256> palette{};
            require(compositor.compose(raster.resident_output(),1,cpu,coverage,settings,palette,nullptr,&background),"terrain composition");
            const auto output=compositor.output();
            command=SDL_AcquireGPUCommandBuffer(device);require(command,"terrain metadata readback");
            const auto composed=read_pixels(device,command,{device,output.packed,nullptr,256,64});
            for(unsigned i=0;i<composed.size();++i) {
                const unsigned x=i%256;
                const bool visible=!(x>=64 && x<128) && x<192;
                require((composed[i]&0x08000000u)==(visible?(expected[i]&0x08000000u):0),"terrain leaked through model or black HUD");
            }
            ink.left=192;ink.right=256;ink.even=ink.odd=0;ink.tag=unsigned(PixelLayer::two_d);native.add(ink);
            const auto snapshot=std::make_shared<const starfox::simulation::SnesPpuState>(*ppu);
            const std::array<GpuSceneDraw,2> terrain_scene{
                GpuBackgroundDraw{snapshot,s,1},GpuRasterDraw{&native,false,false}};
            for(bool separate:{false,true}) {
                if(separate) SDL_setenv_unsafe("STARFOX_TEST_SEPARATE_SCENE_MERGE","1",1);
                else SDL_unsetenv_unsafe("STARFOX_TEST_SEPARATE_SCENE_MERGE");
                GpuScene scene;command=SDL_AcquireGPUCommandBuffer(device);require(command,"terrain scene command");
                const auto merged=scene.enqueue_batch(device,command,256,64,terrain_scene);
                require(merged.pixels,"terrain scene merge");
                const auto metadata=read_pixels(device,command,merged);
                for(unsigned i=0;i<metadata.size();++i) {
                    const unsigned x=i%256;const bool visible=!(x>=64 && x<128) && x<192;
                    require((metadata[i]&0x08000000u)==(visible?(expected[i]&0x08000000u):0),"terrain scene/fused ownership");
                }
            }
            SDL_unsetenv_unsafe("STARFOX_TEST_SEPARATE_SCENE_MERGE");
        }
        {
            // Several native layers share one owned PPU snapshot, with an
            // explicit black foreground write between tile priority passes.
            GpuSceneRecording recording;RasterCommands pending;GpuScene scene;
            recording.reset(800,128);pending.reset(800,128);
            auto snapshot=std::make_shared<const starfox::simulation::SnesPpuState>(*ppu);
            GpuBackgroundSettings s;s.layer=1;s.priority=TilePriorityPass::low;
            recording.append_background(pending,{snapshot,s,2});
            RasterCommand black;black.left=10;black.right=100;black.top=10;black.bottom=30;
            black.tag=unsigned(PixelLayer::two_d);pending.add(black);
            s.layer=3;s.priority=TilePriorityPass::high;
            recording.append_background(pending,{snapshot,s,2});
            s.layer=2;s.scroll_x=-91;s.scroll_y=134;
            recording.append_background(pending,{snapshot,s,2});
            s.layer=1;
            recording.append_background(pending,{snapshot,s,2});recording.finish(pending);
            ppu->vram.fill(0); // Deferred records must retain the earlier source.
            Framebuffer expected(400,64,2),actual(400,64,2);
            expected.enable_layer_tags(true);actual.enable_layer_tags(true);
            recording.replay(expected,nullptr);
            require(scene.render_resident(device,800,128,recording.draws()),scene.status().c_str());
            require(scene.readback(actual,nullptr),"mixed background readback");
            require(actual.pixels()==expected.pixels() && actual.layer_tags()==expected.layer_tags(),"mixed background replay parity");
            const auto eye=stereo_scene_eye(recording.draws(),1,6.4,512);
            require(eye.has_value(),"stereo background records");
            require(scene.render_resident(device,800,128,*eye),"stereo background submission");
            require(scene.readback(actual,nullptr) && actual.pixels()==expected.pixels(),"background convergence plane");
        }
        auto* cmd=SDL_AcquireGPUCommandBuffer(device);require(cmd,"invalid-input command");
        GpuBackgroundSettings bad;bad.layer=4;
        require(!gpu.enqueue(device,cmd,*ppu,32,32,1,bad).pixels,"accepted unsupported BG4");
        bad.layer=1;
        require(!gpu.enqueue(device,cmd,*ppu,31,32,2,bad).pixels,"accepted fractional dimensions");
        require(!gpu.enqueue(device,cmd,*ppu,0,32,1,bad).pixels,"accepted empty width");
        require(!gpu.enqueue(device,cmd,*ppu,4097,32,1,bad).pixels,"accepted oversized width");
        bad.horizontal_origin=INT32_MAX;
        require(!gpu.enqueue(device,cmd,*ppu,32,32,1,bad).pixels,"accepted overflowing origin");
        SDL_CancelGPUCommandBuffer(cmd);
        std::cout<<cases<<" BG1/BG2/BG3 GPU cases, "<<samples<<" packed pixel/coverage samples match CPU; mixed priority/black overlay replay, stereo, invalid inputs and release/reuse pass\n";
        }
        SDL_DestroyGPUDevice(device);SDL_Quit();return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';if(device) SDL_DestroyGPUDevice(device);SDL_Quit();return 1;
    }
}
