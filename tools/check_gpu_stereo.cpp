#include "starfox/render/gpu_scene.hpp"
#include "starfox/render/stereo_output.hpp"
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <source_location>

void require(bool ok,const std::source_location where=std::source_location::current()) {
    if(!ok) throw std::runtime_error("Stereo check line "+std::to_string(where.line())+": "+SDL_GetError());
}
void check_packing(SDL_GPUDevice* device) {
    using namespace starfox::render;
    constexpr unsigned width=8,height=4;
    SDL_GPUTextureCreateInfo info{};
    info.type=SDL_GPU_TEXTURETYPE_2D;info.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width=width;info.height=height;info.layer_count_or_depth=1;info.num_levels=1;
    std::array<SDL_GPUTexture*,2> eyes{};
    for(auto& eye:eyes) {eye=SDL_CreateGPUTexture(device,&info);require(eye);}
    SDL_GPUTransferBufferCreateInfo transfer{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,width*height*8,0};
    auto* download=SDL_CreateGPUTransferBuffer(device,&transfer);require(download);
    for(const auto mode:{StereoOutput::off,StereoOutput::half_sbs,StereoOutput::full_sbs}) {
        const auto layout=stereo_output_layout(mode,width,height).value();
        info.width=layout.width;
        auto* destination=SDL_CreateGPUTexture(device,&info);require(destination);
        auto* command=SDL_AcquireGPUCommandBuffer(device);require(command);
        for(unsigned eye=0;eye<2;++eye) {
            SDL_GPUColorTargetInfo target{};target.texture=eyes[eye];
            target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;
            target.clear_color=eye?SDL_FColor{0,0,1,1}:SDL_FColor{1,0,0,1};
            auto* pass=SDL_BeginGPURenderPass(command,&target,1,nullptr);require(pass);SDL_EndGPURenderPass(pass);
        }
        require(enqueue_stereo_texture_pack(command,eyes[0],eyes[1],destination,mode,width,height));
        auto* copy=SDL_BeginGPUCopyPass(command);require(copy);
        SDL_GPUTextureRegion region{destination,0,0,0,0,0,layout.width,height,1};
        SDL_GPUTextureTransferInfo target{download,0,0,0};
        SDL_DownloadFromGPUTexture(copy,&region,&target);SDL_EndGPUCopyPass(copy);
        auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);require(fence);
        require(SDL_WaitForGPUFences(device,true,&fence,1));SDL_ReleaseGPUFence(device,fence);
        const auto* pixels=static_cast<const unsigned char*>(SDL_MapGPUTransferBuffer(device,download,false));require(pixels);
        bool exact=true;
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<layout.width;++x) {
            const auto i=(y*layout.width+x)*4;
            const bool right=layout.eye_count==2 && x>=layout.eyes[1].x;
            exact &= pixels[i]==(right?0:255) && pixels[i+1]==0
                && pixels[i+2]==(right?255:0) && pixels[i+3]==255;
        }
        SDL_UnmapGPUTransferBuffer(device,download);require(exact);
        SDL_ReleaseGPUTexture(device,destination);
        std::cout<<"GPU packing mode "<<unsigned(mode)<<": "<<layout.width<<"x"<<height<<" exact pixels\n";
    }
    for(auto* eye:eyes) SDL_ReleaseGPUTexture(device,eye);
    SDL_ReleaseGPUTransferBuffer(device,download);
}
int main() try {
    using namespace starfox::render;
    require(SDL_Init(SDL_INIT_VIDEO));
    auto* device=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV|SDL_GPU_SHADERFORMAT_MSL|SDL_GPU_SHADERFORMAT_DXIL,true,nullptr);
    require(device);
    check_packing(device);
    {
        constexpr unsigned width=224,height=192,bytes=width*height*4;
        SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,bytes*2,0};
        auto* download=SDL_CreateGPUTransferBuffer(device,&info);require(download);
        GpuStereoScene scene;
        starfox::assets::Shape shape;
        shape.vertices={{-20,-20,0},{20,-20,0},{0,20,0}};
        // Source span filling requires the cartridge's clockwise face order.
        shape.faces={{-1,0,{0,0,1},{2,1,0}}};shape.colour_words={0x11};
        GpuModelDraw model;model.shape=&shape;
        model.pose.force_colour=true;model.pose.forced_colour=0x11;
        // Exercise reuse and both sides of the zero-disparity plane.
        for(const double depth:{100.,200.,400.,100.}) {
            model.pose.z=depth;
            const std::array<GpuSceneDraw,1> frame{model};
            Framebuffer reference(width,height);
            const auto reference_eye=stereo_scene_eye(frame,0,6.4,200);
            const auto& reference_model=std::get<GpuModelDraw>((*reference_eye)[0]);
            SoftwareRenderer(reference_model.settings).draw(shape,reference_model.pose,reference);
            std::cout<<"CPU coverage "<<std::count_if(reference.pixels().begin(),reference.pixels().end(),[](auto p){return p!=0;})<<'\n';
            // Separate instances exercise owned/fenced submissions without
            // mixing them into the caller-owned enqueue instance's lifetime.
            GpuStereoScene owned;
            require(owned.render_resident(device,width,height,frame,6.4,200));
            auto* command=SDL_AcquireGPUCommandBuffer(device);require(command);
            const auto output=depth==200
                ? std::optional<std::array<GpuRasterOutput,2>>{{owned.resident_output(0),owned.resident_output(1)}}
                : scene.enqueue(device,command,width,height,frame,6.4,200);
            if(!output) {SDL_CancelGPUCommandBuffer(command);throw std::runtime_error("Stereo enqueue failed");}
            require((*output)[0].pixels!=(*output)[1].pixels);
            auto* pass=SDL_BeginGPUCopyPass(command);require(pass);
            for(unsigned eye=0;eye<2;++eye) {
                SDL_GPUBufferRegion from{static_cast<SDL_GPUBuffer*>((*output)[eye].pixels),0,bytes};
                SDL_GPUTransferBufferLocation to{download,eye*bytes};
                SDL_DownloadFromGPUBuffer(pass,&from,&to);
            }
            SDL_EndGPUCopyPass(pass);
            auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);require(fence);
            require(SDL_WaitForGPUFences(device,true,&fence,1));SDL_ReleaseGPUFence(device,fence);
            const auto* pixels=static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device,download,false));require(pixels);
            std::array<double,2> centroid{};
            std::array<unsigned,2> count{};
            bool exact=true;
            for(unsigned eye=0;eye<2;++eye) {
                const auto eye_frame=stereo_scene_eye(frame,eye,6.4,200);
                const auto& draw=std::get<GpuModelDraw>((*eye_frame)[0]);
                SoftwareRenderer(draw.settings).draw(shape,draw.pose,reference,true);
                for(unsigned i=0;i<width*height;++i)
                    exact &= (pixels[eye*width*height+i]&255U)==reference.pixels()[i];
            }
            for(unsigned eye=0;eye<2;++eye) for(unsigned i=0;i<width*height;++i)
                if((pixels[eye*width*height+i]&255U)!=0) {centroid[eye]+=i%width;++count[eye];}
            SDL_UnmapGPUTransferBuffer(device,download);
            require(exact);
            std::cout<<"GPU coverage "<<count[0]<<" / "<<count[1]<<'\n';
            require(count[0]>20 && count[1]>20);
            for(unsigned eye=0;eye<2;++eye) centroid[eye]/=count[eye];
            const double disparity=centroid[0]-centroid[1];
            const double expected=256*6.4*(1/depth-1/200.);
            if(std::abs(disparity-expected)>1) throw std::runtime_error("Native stereo disparity incorrect");
            std::cout<<"Depth "<<depth<<": disparity "<<disparity<<", expected "<<expected<<"; both outputs retained\n";
        }
        scene.release_device();SDL_ReleaseGPUTransferBuffer(device,download);
    }
    SDL_DestroyGPUDevice(device);SDL_Quit();
    std::cout<<"Native GPU stereo submission passed\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
