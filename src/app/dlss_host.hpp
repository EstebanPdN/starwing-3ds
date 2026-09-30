#pragma once
#include "starfox/render/dlss_native.h"
#include "starfox/render/sdl_d3d12_bridge.h"
#include "starfox/render/gpu_temporal_inputs.hpp"
#include "starfox/render/gpu_composite.hpp"
#include "starfox/render/temporal_projection.hpp"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <cmath>

#if defined(_WIN32) && !defined(STARFOX_UWP)
#include <windows.h>
// Default-off integration scaffold. Construct before SDL initialization, but
// finish before destroying the Window, then destroy before SDL_Quit.
class DlssHost {
    HMODULE adapter_{};
    void* sdk_{};
    decltype(&starfox_dlss_close_v1) close_{};
    decltype(&starfox_dlss_bind_device_v1) bind_{};
    decltype(&starfox_dlss_swapchain_v1) swapchain_{};
    StarfoxSdlD3D12PresentHooksV1 hooks_{};
    decltype(&starfox_dlss_configure_v1) configure_{};
    decltype(&starfox_dlss_evaluate_v1) evaluate_{};
    decltype(&starfox_dlss_release_viewport_v1) release_{};
    starfox::render::GpuTemporalInputs guides_;
    SDL_GPUDevice* evaluation_device_{};SDL_GPUTexture* output_{};
    uint32_t width_{},height_{},frame_index_{};uint64_t epoch_{},serial_{};
    bool configured_{};
    uint32_t mode_{4},render_width_{},render_height_{};
    std::optional<starfox::render::TemporalProjection> previous_projection_;
    std::optional<starfox::render::TemporalCamera> previous_camera_;
    std::optional<std::int32_t> previous_ground_height_;
    std::array<float,4> previous_pixel_projection_{};
    // Explicit diagnostic readback only; never enabled by production settings.
    static void audit_terrain(SDL_GPUDevice* device,const starfox::render::GpuCompositeOutput& input,
        const starfox::render::GpuTemporalTextures& guides,const starfox::render::TemporalGroundInputs& ground) {
        const uint32_t count=input.width*input.height;
        const uint32_t depth_offset=(count*4+511u)&~511u;
        const uint32_t pitch=(input.width+63u)&~63u;
        const uint32_t motion_offset=(depth_offset+pitch*input.height*4+511u)&~511u;
        SDL_GPUTransferBufferCreateInfo info{};info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size=motion_offset+pitch*input.height*8;
        auto* transfer=SDL_CreateGPUTransferBuffer(device,&info);
        if(!transfer) throw std::runtime_error(SDL_GetError());
        struct Cleanup {SDL_GPUDevice* device;SDL_GPUTransferBuffer* transfer;
            ~Cleanup(){SDL_ReleaseGPUTransferBuffer(device,transfer);}} cleanup{device,transfer};
        auto* command=SDL_AcquireGPUCommandBuffer(device);
        if(!command) throw std::runtime_error(SDL_GetError());
        auto* pass=SDL_BeginGPUCopyPass(command);
        if(!pass){SDL_CancelGPUCommandBuffer(command);throw std::runtime_error(SDL_GetError());}
        SDL_GPUBufferRegion source{static_cast<SDL_GPUBuffer*>(input.packed),0,count*4};
        SDL_GPUTransferBufferLocation destination{transfer,0};
        SDL_DownloadFromGPUBuffer(pass,&source,&destination);
        SDL_GPUTextureRegion region{};region.texture=static_cast<SDL_GPUTexture*>(guides.depth);
        region.w=input.width;region.h=input.height;region.d=1;
        SDL_GPUTextureTransferInfo texture_destination{transfer,depth_offset,pitch,input.height};
        SDL_DownloadFromGPUTexture(pass,&region,&texture_destination);
        region.texture=static_cast<SDL_GPUTexture*>(guides.motion);texture_destination.offset=motion_offset;
        SDL_DownloadFromGPUTexture(pass,&region,&texture_destination);SDL_EndGPUCopyPass(pass);
        auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);
        if(!fence) throw std::runtime_error(SDL_GetError());
        const bool waited=SDL_WaitForGPUFences(device,true,&fence,1);SDL_ReleaseGPUFence(device,fence);
        if(!waited) throw std::runtime_error(SDL_GetError());
        const auto* mapped=static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device,transfer,false));
        if(!mapped) throw std::runtime_error(SDL_GetError());
        const auto* depth=reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(mapped)+depth_offset);
        const auto* motion=reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(mapped)+motion_offset);
        uint32_t covered=0,valid=0,valid_motion=0,moving=0,mismatches=0;
        for(uint32_t y=0;y<input.height;++y) for(uint32_t x=0;x<input.width;++x)
            if(mapped[y*input.width+x]&0x08000000u) {
                ++covered;const float value=depth[y*pitch+x];
                valid+=std::isfinite(value) && value>=0.f && value<1.f;
                const double px=x+0.5-ground.raster_jitter[0],py=y+0.5-ground.raster_jitter[1];
                const double rx=(px-ground.projection[2])/ground.projection[0];
                const double ry=(py-ground.projection[3])/ground.projection[1];
                const double denominator=rx*ground.plane[0]+ry*ground.plane[1]+ground.plane[2];
                const double z=-ground.plane[3]/denominator;
                const auto& matrix=ground.current_to_previous;
                const double previous_z=rx*z*matrix[2]+ry*z*matrix[6]+z*matrix[10]+matrix[14];
                if(ground.previous_valid && std::isfinite(z) && z>=0.1 && z<=100000 &&
                    std::isfinite(previous_z) && previous_z>=0.1 && previous_z<=100000) {
                    const double previous_x=rx*z*matrix[0]+ry*z*matrix[4]+z*matrix[8]+matrix[12];
                    const double previous_y=rx*z*matrix[1]+ry*z*matrix[5]+z*matrix[9]+matrix[13];
                    const double dx=previous_x/previous_z*ground.previous_projection[0]+ground.previous_projection[2]-px;
                    const double dy=previous_y/previous_z*ground.previous_projection[1]+ground.previous_projection[3]-py;
                    const float mx=motion[(y*pitch+x)*2],my=motion[(y*pitch+x)*2+1];
                    const bool matches=std::isfinite(mx) && std::isfinite(my) && std::abs(mx-dx)<0.02 && std::abs(my-dy)<0.02;
                    valid_motion+=matches;mismatches+=!matches;
                    moving+=matches && (std::abs(mx)>0.001 || std::abs(my)>0.001);
                }
            }
        SDL_UnmapGPUTransferBuffer(device,transfer);
        std::cerr<<"dlss-terrain-audit: covered="<<covered<<" valid_depth="<<valid
            <<" valid_motion="<<valid_motion<<" moving="<<moving<<" mismatches="<<mismatches<<'\n';
        if(!covered || !valid) throw std::runtime_error("Diagnostic scene has no usable terrain depth");
        if(!valid_motion || mismatches) throw std::runtime_error("Diagnostic terrain reprojection mismatch");
    }
public:
    DlssHost() {
        if(!std::getenv("STARFOX_TEST_DLSS_LIFECYCLE")) return;
        try {
            const auto* adapter=std::getenv("STARFOX_DLSS_ADAPTER");
            const auto* binaries=std::getenv("STARFOX_DLSS_BINARIES");
            if(!adapter || !binaries) throw std::runtime_error("Set explicit adapter and official binary paths");
            const auto adapter_path=std::filesystem::path(adapter),binary_path=std::filesystem::path(binaries);
            if(!adapter_path.is_absolute() || !binary_path.is_absolute()) throw std::runtime_error("DLSS paths must be absolute");
            adapter_=LoadLibraryExW(adapter_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!adapter_) throw std::runtime_error("Cannot load optional DLSS adapter");
            auto open=reinterpret_cast<decltype(&starfox_dlss_open_v1)>(GetProcAddress(adapter_,"starfox_dlss_open_v1"));
            close_=reinterpret_cast<decltype(close_)>(GetProcAddress(adapter_,"starfox_dlss_close_v1"));
            bind_=reinterpret_cast<decltype(bind_)>(GetProcAddress(adapter_,"starfox_dlss_bind_device_v1"));
            swapchain_=reinterpret_cast<decltype(swapchain_)>(GetProcAddress(adapter_,"starfox_dlss_swapchain_v1"));
            configure_=reinterpret_cast<decltype(configure_)>(GetProcAddress(adapter_,"starfox_dlss_configure_v1"));
            evaluate_=reinterpret_cast<decltype(evaluate_)>(GetProcAddress(adapter_,"starfox_dlss_evaluate_v1"));
            release_=reinterpret_cast<decltype(release_)>(GetProcAddress(adapter_,"starfox_dlss_release_viewport_v1"));
            if(!open || !close_ || !bind_ || !swapchain_ || !configure_ || !evaluate_ || !release_) throw std::runtime_error("DLSS adapter lifecycle ABI missing");
            char error[512]{};
            if(open(binary_path.c_str(),&sdk_,error,sizeof(error))) throw std::runtime_error(error);
            hooks_={1,this,[](void* user,void* device,void** chain,bool restore)->bool {
                auto& self=*static_cast<DlssHost*>(user);if(!self.sdk_) return true;
                char message[512]{};
                if((!restore && self.bind_(self.sdk_,device,message,sizeof(message))) ||
                    self.swapchain_(self.sdk_,chain,restore?1:0,message,sizeof(message))) {
                    std::cerr<<"dlss-presentation: "<<message<<'\n';return false;
                }
                std::cerr<<"dlss-presentation: "<<(restore?"restored":"upgraded")<<'\n';return true;
            }};
            SDL_SetPointerProperty(SDL_GetGlobalProperties(),STARFOX_SDL_D3D12_PRESENT_HOOKS,&hooks_);
            std::cerr<<"dlss-lifecycle: initialized before SDL; evaluation disabled\n";
        } catch(const std::exception& e) {
            std::cerr<<"dlss-lifecycle: unavailable: "<<e.what()<<'\n';
            if(adapter_) FreeLibrary(adapter_);adapter_=nullptr;
        }
    }
    ~DlssHost() {
        if(SDL_GetPointerProperty(SDL_GetGlobalProperties(),STARFOX_SDL_D3D12_PRESENT_HOOKS,nullptr)==&hooks_)
            SDL_ClearProperty(SDL_GetGlobalProperties(),STARFOX_SDL_D3D12_PRESENT_HOOKS);
        if(sdk_) {
            char error[512]{};
            if(close_(sdk_,error,sizeof(error))) {
                // Do not unload code/resources after failed SDK shutdown.
                std::cerr<<"dlss-lifecycle: shutdown failed: "<<error<<'\n';return;
            }
            std::cerr<<"dlss-lifecycle: shutdown before SDL\n";
        }
        if(adapter_) FreeLibrary(adapter_);
    }
    DlssHost(const DlssHost&)=delete;
    DlssHost& operator=(const DlssHost&)=delete;
    void finish(SDL_Renderer* renderer) {
        if(!sdk_) return;
        auto* gpu=static_cast<SDL_GPUDevice*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),SDL_PROP_RENDERER_GPU_DEVICE_POINTER,nullptr));
        if(gpu && !SDL_WaitForGPUIdle(gpu)) {std::cerr<<"dlss-lifecycle: GPU idle failed\n";return;}
        guides_.release_device();
        if(output_) {SDL_ReleaseGPUTexture(evaluation_device_,output_);output_=nullptr;}
        char viewport_error[512]{};
        if(configured_ && release_(sdk_,99,viewport_error,sizeof(viewport_error))) {std::cerr<<viewport_error<<'\n';return;}
        configured_=false;
        if(gpu) {
            auto* bridge=static_cast<const StarfoxSdlD3D12PresentBridgeV1*>(SDL_GetPointerProperty(SDL_GetGPUDeviceProperties(gpu),STARFOX_SDL_D3D12_PRESENT_BRIDGE,nullptr));
            if(bridge && !bridge->restore(gpu)) {std::cerr<<"dlss-lifecycle: swapchain restore failed\n";return;}
        }
        char error[512]{};
        if(close_(sdk_,error,sizeof(error))) {std::cerr<<"dlss-lifecycle: shutdown failed: "<<error<<'\n';return;}
        sdk_=nullptr;std::cerr<<"dlss-lifecycle: shutdown before renderer destruction\n";
    }
    void bind(SDL_Renderer* renderer) {
        if(!sdk_) return;
        auto* gpu=static_cast<SDL_GPUDevice*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),SDL_PROP_RENDERER_GPU_DEVICE_POINTER,nullptr));
        auto* native=gpu?SDL_GetPointerProperty(SDL_GetGPUDeviceProperties(gpu),STARFOX_SDL_D3D12_DEVICE,nullptr):nullptr;
        char error[512]{};
        if(!native) std::cerr<<"dlss-lifecycle: current renderer is not native D3D12\n";
        else if(bind_(sdk_,native,error,sizeof(error))) std::cerr<<"dlss-lifecycle: adapter unsupported: "<<error<<'\n';
        else std::cerr<<"dlss-lifecycle: actual game GPU bound; evaluation disabled\n";
    }
    starfox::render::GpuCompositeOutput evaluate(const starfox::render::GpuCompositeOutput& input,
        float focal,float cx,float cy,uint64_t serial,uint64_t epoch,const starfox::render::GpuCompositeOutput* final=nullptr,
        const starfox::render::TemporalCamera* camera=nullptr,const starfox::render::TemporalGroundPlane* ground_plane=nullptr) {
        const auto& original=final?*final:input;
        if(!sdk_ || !std::getenv("STARFOX_TEST_DLSS_EVALUATE") || !input.geometry_depth) return original;
        auto* device=static_cast<SDL_GPUDevice*>(input.device);
        if(evaluation_device_ && evaluation_device_!=device) return original;
        auto* bridge=static_cast<const StarfoxSdlD3D12ComputeBridgeV1*>(SDL_GetPointerProperty(SDL_GetGPUDeviceProperties(device),STARFOX_SDL_D3D12_COMPUTE_BRIDGE,nullptr));
        if(!bridge || !std::isfinite(focal) || focal<=0) return original;
        SDL_GPUCommandBuffer* command{};
        try {
            auto checked=[](bool ok,const char* error){if(!ok) throw std::runtime_error(error);};
            uint32_t requested_mode=4;
            if(const auto* value=std::getenv("STARFOX_TEST_DLSS_MODE")) {
                const std::string_view name{value};
                if(name=="QUALITY") requested_mode=1;else if(name=="BALANCED") requested_mode=2;
                else if(name=="PERFORMANCE") requested_mode=3;else checked(name=="DLAA","Unknown DLSS mode");
            }
            bool reset=serial_+1!=serial || epoch_!=epoch || !configured_ || !input.motion || !previous_camera_;
            if(width_!=input.width || height_!=input.height || requested_mode!=mode_ || !configured_ || !output_) {
                checked(SDL_WaitForGPUIdle(device),SDL_GetError());char error[512]{};
                if(configured_) checked(!release_(sdk_,99,error,sizeof(error)),error);
                configured_=false;
                uint32_t w{},h{};checked(!configure_(sdk_,99,requested_mode,input.width,input.height,&w,&h,error,sizeof(error)),error);
                configured_=true;checked(w && h && w<=input.width && h<=input.height,"Invalid DLSS render dimensions");
                render_width_=w;render_height_=h;mode_=requested_mode;
                if(output_) SDL_ReleaseGPUTexture(device,output_);output_=nullptr;
                SDL_GPUTextureCreateInfo info{};info.type=SDL_GPU_TEXTURETYPE_2D;info.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                info.usage=SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_TEXTUREUSAGE_SAMPLER;
                info.width=input.width;info.height=input.height;info.layer_count_or_depth=1;info.num_levels=1;
                output_=SDL_CreateGPUTexture(device,&info);checked(output_,SDL_GetError());
                width_=input.width;height_=input.height;evaluation_device_=device;reset=true;
            }
            command=SDL_AcquireGPUCommandBuffer(device);checked(command,SDL_GetError());
            constexpr float near_plane=.1f,far_plane=100000.f;
            starfox::render::TemporalGroundInputs ground;
            ground.coverage=input.packed;ground.packed_coverage=true;
            ground.projection={focal,focal,cx,cy};ground.previous_projection=ground.projection;
            if(ground_plane) ground.plane=ground_plane->camera_plane;
            StarfoxDlssFrameV1 frame{};frame.size=sizeof(frame);frame.viewport=99;frame.frame_index=frame_index_++;
            frame.width=frame.output_width=input.width;frame.height=frame.output_height=input.height;frame.reset=reset;
            const auto projection=starfox::render::temporal_projection(input.width,input.height,focal,cx,cy,near_plane,far_plane);
            checked(bool(projection),"Invalid temporal camera projection");
            std::copy(projection->view_to_clip.begin(),projection->view_to_clip.end(),frame.view_to_clip);
            std::copy(projection->clip_to_view.begin(),projection->clip_to_view.end(),frame.clip_to_view);
            checked(camera!=nullptr,"Missing temporal camera");
            const starfox::render::TemporalCamera origin;
            const auto view_to_world=starfox::render::temporal_camera_mapping(*camera,origin);
            checked(bool(view_to_world),"Invalid temporal camera");
            if(!reset && previous_projection_ && previous_camera_) {
                const auto backward_camera=starfox::render::temporal_camera_mapping(*camera,*previous_camera_);
                const auto forward_camera=starfox::render::temporal_camera_mapping(*previous_camera_,*camera);
                checked(bool(backward_camera) && bool(forward_camera),"Invalid camera history");
                if(ground_plane && previous_ground_height_==ground_plane->world_height) {
                    ground.previous_valid=true;ground.previous_projection=previous_pixel_projection_;
                    ground.current_to_previous=*backward_camera;
                }
                const auto back=starfox::render::temporal_matrix_product(starfox::render::temporal_matrix_product(projection->clip_to_view,*backward_camera),previous_projection_->view_to_clip);
                const auto forward=starfox::render::temporal_matrix_product(starfox::render::temporal_matrix_product(previous_projection_->clip_to_view,*forward_camera),projection->view_to_clip);
                std::copy(back.begin(),back.end(),frame.clip_to_previous);
                std::copy(forward.begin(),forward.end(),frame.previous_to_clip);
            } else for(unsigned i=0;i<4;++i) frame.clip_to_previous[i*5]=frame.previous_to_clip[i*5]=1;
            const auto right=starfox::render::temporal_unit_axis(*view_to_world,0);
            const auto up=starfox::render::temporal_unit_axis(*view_to_world,1,-1);
            const auto forward_axis=starfox::render::temporal_unit_axis(*view_to_world,2);
            checked(bool(right) && bool(up) && bool(forward_axis),"Invalid temporal camera axes");
            for(unsigned i=0;i<3;++i) {
                frame.camera_position[i]=float(camera->position[i]);
                frame.camera_right[i]=(*right)[i];
                frame.camera_up[i]=(*up)[i];
                frame.camera_forward[i]=(*forward_axis)[i];
            }
            frame.near_plane=near_plane;frame.far_plane=far_plane;frame.vertical_fov=projection->vertical_fov;frame.aspect=projection->aspect;
            const auto textures=guides_.enqueue(device,command,input.geometry_depth,input.motion,input.width,input.height,near_plane,far_plane,reset,ground_plane?&ground:nullptr);
            checked(textures.depth,guides_.status().c_str());
            auto evaluation_textures=textures;void* evaluation_color=input.rgba;
            if(render_width_!=input.width || render_height_!=input.height) {
                const auto reduced=guides_.resample(device,command,input.rgba,textures,render_width_,render_height_);
                checked(reduced.color,guides_.status().c_str());evaluation_color=reduced.color;evaluation_textures=reduced.guides;
            }
            frame.width=render_width_;frame.height=render_height_;
            struct Callback {DlssHost* host;StarfoxDlssFrameV1* frame;char error[512]{};} callback{this,&frame};
            void* resources[]{evaluation_color,evaluation_textures.depth,evaluation_textures.motion,output_,evaluation_textures.exposure};
            checked(bridge->dispatch(command,resources,5,3,[](void* user,void* list,void* const* textures,uint32_t count)->bool {
                auto& c=*static_cast<Callback*>(user);if(count!=5) return false;
                auto& f=*c.frame;f.command=list;f.color=textures[0];f.depth=textures[1];f.motion=textures[2];f.output=textures[3];f.exposure=textures[4];
                // D3D12 NON_PIXEL_SHADER_RESOURCE=0x40, UNORDERED_ACCESS=0x8.
                for(unsigned i=0;i<5;++i) f.states[i]=i==3?0x8u:0x40u;
                return c.host->evaluate_(c.host->sdk_,&f,c.error,sizeof(c.error))==0;
            },&callback),callback.error);
            auto* protected_output=guides_.restore_hud(device,command,original.rgba,output_,original.packed,input.width,input.height);
            checked(protected_output,guides_.status().c_str());
            // The native callback, HUD restore, effects and presentation all use
            // this SDL device's one D3D12 command queue. Submission order and
            // resource transitions order reuse; a CPU fence wait here needlessly
            // serialized every frame. Reconfiguration/shutdown still wait idle.
            const bool submitted=SDL_SubmitGPUCommandBuffer(command);command=nullptr;
            checked(submitted,SDL_GetError());
            if(std::getenv("STARFOX_TEST_DLSS_AUDIT_TERRAIN") && (frame.frame_index==1 || frame.frame_index%16==15))
                audit_terrain(device,input,textures,ground);
            // Explicit diagnostic comparison only, never a normal-frame stall.
            if(std::getenv("STARFOX_TEST_DLSS_SERIALIZE"))
                checked(SDL_WaitForGPUIdle(device),SDL_GetError());
            serial_=serial;epoch_=epoch;previous_projection_=projection;previous_camera_=*camera;
            previous_pixel_projection_=ground.projection;
            previous_ground_height_=ground_plane?std::optional<std::int32_t>(ground_plane->world_height):std::nullopt;
            std::cerr<<"dlss-gameplay: evaluated frame="<<frame.frame_index<<" reset="<<reset<<" size="<<input.width<<'x'<<input.height
                <<" mode="<<mode_<<" render="<<render_width_<<'x'<<render_height_<<" diagnostic, incomplete world inputs\n";
            auto result=original;result.rgba=protected_output;return result;
        } catch(const std::exception& e) {
            if(command) SDL_CancelGPUCommandBuffer(command);serial_=0;
            std::cerr<<"dlss-gameplay: failed: "<<e.what()<<'\n';return original;
        }
    }
};
#else
class DlssHost {public:void bind(SDL_Renderer*) {} void finish(SDL_Renderer*) {}
    starfox::render::GpuCompositeOutput evaluate(const starfox::render::GpuCompositeOutput& input,float,float,float,uint64_t,uint64_t,const starfox::render::GpuCompositeOutput* final=nullptr,const starfox::render::TemporalCamera* =nullptr,const starfox::render::TemporalGroundPlane* =nullptr){return final?*final:input;}};
#endif
