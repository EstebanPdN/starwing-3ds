#include "starfox/render/dxr_shadows.hpp"
#if defined(STARFOX_TEST_PORTABLE_SHADOWS)
#include "starfox/render/portable_shadows.hpp"
#include "starfox/render/sdl_gpu_effects.hpp"
#include <SDL3/SDL.h>
#endif
#include <chrono>
#include <iostream>
#include <algorithm>
#include <array>
#include <iomanip>
#if defined(_WIN32) && !defined(STARFOX_TEST_PORTABLE_SHADOWS)
#include <windows.h>
#include <d3d12.h>
#endif
int main() {
    using namespace starfox::render::shadows;
#if defined(STARFOX_TEST_PORTABLE_SHADOWS)
    if(!SDL_Init(SDL_INIT_VIDEO)) return 3;
    struct SdlLifetime { ~SdlLifetime(){SDL_Quit();} } sdl;
#endif
    Scene scene;
    for (int z=0;z<8;++z) for (int x=-4;x<4;++x) {
        const Vec3 a{x*18.0,-12,z*25.0+60};
        scene.add({a,a+Vec3{14,0,0},a+Vec3{7,20,4}});
        scene.add({a,a+Vec3{7,20,4},a+Vec3{0,0,12}});
    }
    scene.build();
#if defined(STARFOX_TEST_PORTABLE_SHADOWS)
    PortableShadows gpu;
#else
    DxrShadows gpu;
#endif
    starfox::render::RowWorkers workers; workers.set_worker_count(4);
    for (unsigned scale:{1U,2U,4U}) {
        const Camera camera{400*scale,224*scale,256.0*scale,200.0*scale,112.0*scale};
        const ReceiverPlane ground{{0,25,0},{0,1,0}};
        std::vector<std::uint8_t> cpu,hardware;
        std::vector<double> cpu_times;
        for(unsigned i=0;i<5;++i) {
            const auto start=std::chrono::steady_clock::now();
            render_mask(scene,camera,{-1,-1,-1},ground,cpu,&workers);
            if(i) cpu_times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        std::sort(cpu_times.begin(),cpu_times.end());
        std::vector<double> times;
        for (unsigned i=0;i<12;++i) {
            const auto start=std::chrono::steady_clock::now();
            if (!gpu.render(scene,camera,{-1,-1,-1},ground,hardware)) {
                std::cerr << gpu.status() << '\n'; return 1;
            }
            if(i>=2) times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        std::sort(times.begin(),times.end());
        if (hardware.size()!=cpu.size()) return 11;
        const auto shadowed=std::count_if(hardware.begin(),hardware.end(),
            [](std::uint8_t value){return value!=0;});
        // Matching empty masks are not evidence of working ray tracing.
        if(shadowed==0) {std::cerr << "No shadow rays affected the fixture\n";return 12;}
        std::size_t different=0; unsigned largest=0;
        for(std::size_t i=0;i<cpu.size();++i) {
            different+=cpu[i]!=hardware[i];
            largest=std::max(largest,unsigned(std::abs(int(cpu[i])-int(hardware[i]))));
        }
        std::cout << gpu.status() << " scale=" << scale << " median_ms=" << times[times.size()/2]
            << " cpu_median_ms=" << cpu_times[cpu_times.size()/2]
            << " shadowed_pixels=" << shadowed
            << " differing_pixels=" << different << '/' << cpu.size() << " max_delta=" << largest << '\n';
        if (different>cpu.size()/1000) return 2;
    }
    // Benchmark changing geometry as well: static inputs reuse the AS and
    // conceal the work performed for moving ships/enemies during gameplay.
#if !defined(STARFOX_TEST_PORTABLE_SHADOWS)
    {
        const Camera camera{800,448,512,400,224};
        const ReceiverPlane ground{{0,25,0},{0,1,0}};
        const auto original=scene.triangles();
        const std::vector<Triangle> source(original.begin(),original.end());
        std::vector<double> downloaded_times,resident_times;
        DxrShadows resident;
        for(unsigned frame=0;frame<32;++frame) {
            const Vec3 shift{double(frame%7)*0.125,0,double(frame)*0.0625};
            Scene animated;
            for(const auto& triangle:source)
                animated.add({triangle.a+shift,triangle.b+shift,triangle.c+shift});
            animated.build();
            // Separate producers prevent the second path reusing the first
            // path's just-built geometry and biasing the comparison.
            std::vector<std::uint8_t> downloaded,read;
            auto start=std::chrono::steady_clock::now();
            if(!gpu.render(animated,camera,{-1,-1,-1},ground,downloaded)) return 31;
            const auto download_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            start=std::chrono::steady_clock::now();
            if(!resident.render_resident(animated,camera,{-1,-1,-1},ground)) return 32;
            const auto resident_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(!resident.readback_resident(read) || read!=downloaded) return 33;
            if(frame>=8) {downloaded_times.push_back(download_ms);resident_times.push_back(resident_ms);}
        }
        std::sort(downloaded_times.begin(),downloaded_times.end());
        std::sort(resident_times.begin(),resident_times.end());
        std::cout<<"Animated DXR 2x: download_median_ms="<<downloaded_times[downloaded_times.size()/2]
            <<" resident_median_ms="<<resident_times[resident_times.size()/2]
            <<"; 32 exact mask comparisons passed (resident timing excludes diagnostic download)\n";
    }
#endif
    // Rebuild and clear the same object to catch accidental cross-frame
    // geometry reuse. Exercise a different size, light axis and receiver.
    for(unsigned variant=0;variant<6;++variant) {
        scene.clear();
        if(variant!=5) {
            const double z=40+variant*11;
            scene.add({{-20,-10,z},{20,-10,z},{0,20,z+6}});
            scene.add({{0,20,z+6},{20,-10,z},{-20,-10,z}});
        }
        scene.build();
        const Camera camera{133+variant,79+variant,100,64,35};
        const Vec3 light=variant%2?Vec3{0,-1,0}:Vec3{-1,-1,-1};
        std::optional<ReceiverPlane> ground;
        if(variant%2) ground=ReceiverPlane{{0,25,0},{0,1,0}};
        std::vector<std::uint8_t> cpu,hardware;
        render_mask(scene,camera,light,ground,cpu,&workers);
        if(!gpu.render(scene,camera,light,ground,hardware) || hardware.size()!=cpu.size()) return 4;
        std::size_t differences=0;
        for(std::size_t i=0;i<cpu.size();++i) differences+=cpu[i]!=hardware[i];
        if(differences>cpu.size()/1000) return 5;
    }
    std::cout<<"Changing geometry, size, light, winding, receiver and empty scene: passed\n";
    // A nonempty mask could contain only model self-shadowing. Prove that a
    // detached caster also shades the ground outside its screen silhouette.
    scene.clear();
    scene.add({{-20,0,80},{20,0,80},{20,0,120}});
    scene.add({{-20,0,80},{20,0,120},{-20,0,120}});
    scene.build();
    {
        const Camera camera{133,79,100,64,35};
        const ReceiverPlane ground{{0,25,0},{0,1,0}};
        std::vector<std::uint8_t> mask,without_ground;
        if(!gpu.render(scene,camera,{0,-1,0},ground,mask)
            || !gpu.render(scene,camera,{0,-1,0},{},without_ground)
            || mask.size()!=133U*79U || without_ground.size()!=mask.size()
            || mask[60U*133U+64U]!=160U
            || without_ground[60U*133U+64U]!=0U
            || mask[10U*133U+64U]!=0U
            || mask[60U*133U+4U]!=0U) {
            std::cerr<<"Detached caster failed ground-only shadow/sky isolation\n";
            return 15;
        }
    }
    std::cout<<"Detached ground shadow, absent receiver and unshadowed sky: passed\n";
    // Exercise partial workgroups and every packed-row remainder, including
    // one-pixel rows. A later render must not retain another size's padding.
    scene.clear();
    scene.add({{-20,-10,40},{20,-10,40},{0,20,46}});
    scene.add({{0,20,46},{20,-10,40},{-20,-10,40}});
    scene.build();
    for(unsigned width=1;width<=17;++width) for(unsigned height:{1U,3U,9U}) {
        const Camera camera{width,height,12,double(width)/2,double(height)/2};
        std::vector<std::uint8_t> cpu,hardware;
        render_mask(scene,camera,{-1,-1,-1},ReceiverPlane{{0,25,0},{0,1,0}},cpu,&workers);
        if(!gpu.render(scene,camera,{-1,-1,-1},ReceiverPlane{{0,25,0},{0,1,0}},hardware)
            || hardware!=cpu) {
            std::cerr << "Partial shadow workgroup mismatch: " << width << 'x' << height << '\n';
            return 13;
        }
    }
    std::cout<<"All 51 partial-workgroup/packed-row fixtures passed\n";
    for(double receiver_depth:{384.,512.,768.}) {
        std::array<std::vector<std::uint8_t>,2> eye_masks;
        for(unsigned eye=0;eye<2;++eye) {
            const double eye_x=eye?4.:-4.;
            Scene eye_scene;
            const Vec3 offset{-eye_x,0,0};
            eye_scene.add({Vec3{-20,-20,256}+offset,Vec3{20,-20,256}+offset,Vec3{20,20,256}+offset});
            eye_scene.add({Vec3{-20,-20,256}+offset,Vec3{20,20,256}+offset,Vec3{-20,20,256}+offset});
            eye_scene.build();
            const Camera camera{400,224,256,200+256*eye_x/512.,112};
            const ReceiverPlane receiver{{-eye_x,0,receiver_depth},{0,0,-1}};
            std::vector<std::uint8_t> reference;
            render_mask(eye_scene,camera,{-1,0,-1},receiver,reference,&workers);
            if(!gpu.render(eye_scene,camera,{-1,0,-1},receiver,eye_masks[eye]) || eye_masks[eye]!=reference) {
                std::cerr<<"Stereo receiver GPU/CPU mismatch\n";return 15;
            }
            // Independent world-space projection of the displaced shadow's
            // centre onto the receiver, away from the visible caster itself.
            const double world_x=receiver_depth-256.;
            const int x=int(200+256*(world_x-eye_x)/receiver_depth+256*eye_x/512.);
            if(!eye_masks[eye][112*400+unsigned(x)]) {
                std::cerr<<"Stereo shadow missing at projected receiver\n";return 16;
            }
        }
        if(receiver_depth==512.) {
            // Only compare the receiver patch; caster silhouettes have their
            // own depth and correctly retain stereo disparity.
            for(unsigned y=105;y<119;++y) for(unsigned x=320;x<337;++x)
                if(eye_masks[0][y*400+x]!=eye_masks[1][y*400+x]) return 17;
        }
    }
    std::cout<<"Stereo shadow receiver near/convergence/far CPU/GPU checks passed\n";
#if !defined(STARFOX_TEST_PORTABLE_SHADOWS)
    // A new backend must build its AS. Compare that independent result with
    // the reused backend while alternating stable and changed input geometry.
    for(unsigned variant=0;variant<12;++variant) {
        Scene casters;
        const double z=80+16*(variant/3%2);
        casters.add({{-20,0,z},{20,0,z},{0,0,z+40}});casters.build();
        const Camera camera{133+variant,79,100,64+double(variant),35};
        const ReceiverPlane ground{{0,25,0},{0,1,0}};
        const Vec3 light{variant%2?-.4:.4,-1,0};
        DxrShadows fresh;
        std::vector<std::uint8_t> rebuilt,reused;
        if(!fresh.render(casters,camera,light,ground,rebuilt)
            || !gpu.render(casters,camera,light,ground,reused) || rebuilt!=reused) return 23;
    }
    std::cout<<"Cached acceleration structures match fresh builds across geometry/camera/light changes\n";
    for(unsigned variant=0;variant<7;++variant) {
        const Camera camera{133+variant*3,79+variant*2,100,64,35};
        const ReceiverPlane ground{{0,25,0},{0,1,0}};
        std::vector<std::uint8_t> expected,read;
        if(!gpu.render(scene,camera,{-1,-1,-1},ground,expected)
            || gpu.resident_output().resource
            || !gpu.render_resident(scene,camera,{-1,-1,-1},ground)) return 18;
        const auto output=gpu.resident_output();
        if(variant==0) {
            std::cout<<"DXR producer LUID=";
            for(const auto byte:output.adapter_luid) std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
            std::cout<<std::dec<<'\n';
        }
        if(!output.device || !output.resource || output.width!=camera.width
            || output.height!=camera.height || output.row_bytes!=((camera.width+3U)&~3U)) return 19;
#if defined(_WIN32)
        const auto handle=gpu.export_resident_handle();
        if(!handle) return 24;
        ID3D12Resource* imported{};
        const auto opened=static_cast<ID3D12Device*>(output.device)->OpenSharedHandle(handle,
            IID_ID3D12Resource,reinterpret_cast<void**>(&imported));
        CloseHandle(handle);
        if(FAILED(opened) || !imported) return 25;
        const auto description=imported->GetDesc();
        imported->Release();
        if(description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER
            || description.Width<uint64_t(output.row_bytes)*output.height) return 26;
        const auto ready=gpu.export_ready_fence_handle();
        if(!ready || gpu.resident_output().ready_value<=output.ready_value) return 28;
        CloseHandle(ready);
        const auto stable_value=gpu.resident_output().ready_value;
        const auto repeated=gpu.export_ready_fence_handle();
        if(!repeated || gpu.resident_output().ready_value!=stable_value) return 29;
        CloseHandle(repeated);
#endif
        if(!gpu.readback_resident(read) || read!=expected
            || !gpu.readback_resident(read) || read!=expected) return 20;
        if(!gpu.render_resident(scene,camera,{-1,-1,-1},ground,nullptr,nullptr,true,true)) return 34;
        const auto released=gpu.resident_output();
#if defined(_WIN32)
        const auto folded_fence=gpu.export_ready_fence_handle();
        if(!folded_fence || gpu.resident_output().ready_value!=released.ready_value) return 35;
        CloseHandle(folded_fence);
#endif
        if(!gpu.readback_resident(read) || read!=expected) return 36;
        if(gpu.render_resident(scene,camera,{-1,-1,-1},ground,nullptr,nullptr,false,true)) return 37;
        // Reuse immediately after a deferred dispatch, without a consumer
        // download incidentally waiting for the producer first.
        if(!gpu.render_resident(scene,camera,{-1,-1,-1},ground,nullptr,nullptr,true,true)
            || !gpu.render_resident(scene,camera,{-1,-1,-1},ground,nullptr,nullptr,true,true)
            || !gpu.readback_resident(read) || read!=expected) return 38;
    }
    Scene empty_scene;empty_scene.build();
    std::vector<std::uint8_t> stale{1,2,3};
    if(gpu.render_resident(empty_scene,{133,79,100,64,35},{-1,-1,-1},{})
        || gpu.resident_output().resource || gpu.readback_resident(stale) || !stale.empty()) return 21;
    if(gpu.export_resident_handle() || gpu.export_ready_fence_handle()) return 27;
    if(!gpu.render_resident(scene,{133,79,100,64,35},{-1,-1,-1},{})
        || !gpu.readback_resident(stale) || stale.size()!=133*79) return 22;
    std::cout<<"DXR resident masks: exact repeated downloads, folded external release, resize, invalidation and recovery passed\n";
#endif
#if defined(STARFOX_TEST_PORTABLE_SHADOWS)
    struct Device {
        SDL_GPUDevice* value=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV|SDL_GPU_SHADERFORMAT_MSL|SDL_GPU_SHADERFORMAT_DXIL,false,nullptr);
        ~Device(){if(value) SDL_DestroyGPUDevice(value);}
    } borrowed;
    if(!borrowed.value) return 6;
    PortableShadows resident;
    starfox::render::SdlGpuEffects effects;
    for(unsigned variant=0;variant<5;++variant) {
        scene.clear();
        const double z=45+variant*8;
        scene.add({{-20,-10,z},{20,-10,z},{0,20,z+6}});scene.build();
        const Camera camera{133+variant*3,79+variant*2,100,64,35};
        const ReceiverPlane ground{{0,25,0},{0,1,0}};
        std::vector<std::uint8_t> mask,read;
        if(!gpu.render(scene,camera,{-1,-1,-1},ground,mask)
            || !resident.render_resident(borrowed.value,scene,camera,{-1,-1,-1},ground)) return 7;
        const auto output=resident.output();
        if(output.device!=borrowed.value || !output.buffer || output.width!=camera.width
            || output.height!=camera.height) return 8;
        if(!resident.readback(read) || read!=mask || !resident.readback(read) || read!=mask) return 9;
        starfox::render::Framebuffer frame(camera.width,camera.height+4);
        frame.enable_layer_tags(true);
        for(unsigned y=0;y<frame.height();++y) for(unsigned x=0;x<frame.width();++x)
            frame.set_stored(x,y,1,static_cast<starfox::render::PixelLayer>(x%5));
        std::vector<std::uint8_t> expected(frame.pixels().size()*4,200),direct=expected;
        starfox::render::GpuEffectSettings settings;
        settings.shadow_mask=mask;settings.shadow_width=camera.width;settings.shadow_height=camera.height;
        settings.shadow_offset_y=int(variant)-2;
        if(!effects.apply(borrowed.value,frame,expected,settings)) return 10;
        settings.shadow_mask={};settings.resident_shadow=output;
        if(!effects.apply(borrowed.value,frame,direct,settings) || direct!=expected) return 11;
    }
    scene.clear();scene.build();
    if(resident.render_resident(borrowed.value,scene,{133,79,100,64,35},{-1,-1,-1},{})
        || resident.output().buffer) return 12;
    effects.release_device();resident.release_device();
    if(resident.output().buffer || !SDL_GetGPUShaderFormats(borrowed.value)) return 13;
    scene.add({{-20,-10,45},{20,-10,45},{0,20,51}});scene.build();
    std::vector<std::uint8_t> restored;
    if(!resident.render_resident(borrowed.value,scene,{133,79,100,64,35},{-1,-1,-1},{})
        || !resident.readback(restored)) return 14;
    std::cout<<"Resident shadow masks, blend offsets/tags, invalidation and borrowed lifetime: exact\n";
#endif
}
