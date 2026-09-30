#include <SDL3/SDL.h>
#include "starfox/render/gpu_colour_warp.hpp"
#include "starfox/render/raster_commands.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using U=Uint32;using Four=std::array<U,4>;
using namespace starfox::render;
void check(bool v){if(!v)throw std::runtime_error(SDL_GetError());}
int main()try {
    check(SDL_Init(SDL_INIT_VIDEO));auto* device=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,true,nullptr);check(device);
    GpuColourWarp warp;
    std::array<U,4> order{0,0,0,0};std::array<U,2> traversal{4,0};
    Four polygon{0,3,0,0};std::array<Four,3> corners{{{5,0,0,0},{7,0,0,0},{9,0,0,0}}};U visible=1;
    RasterCommand material{};material.has_surface=1;material.surface={0,1,0,100};
    Four normal{};std::array<unsigned char,2480> diffuse{};std::array<unsigned char,128> depth{};
    std::vector<U> lookup(65536,UINT32_MAX);Four texture{};std::array<U,8> coordinates{};
    const std::array<U,12> sizes{16,8,16,48,4,96,16,2480,128,262144,16,32};
    std::array<SDL_GPUBuffer*,12> buffers{};U total=0;
    for(U i=0;i<12;++i){SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,sizes[i],0};buffers[i]=SDL_CreateGPUBuffer(device,&info);check(buffers[i]);total+=sizes[i];}
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,total,0};auto* upload=SDL_CreateGPUTransferBuffer(device,&ti);check(upload);
    ti.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;ti.size=64+2048+384+8;auto* download=SDL_CreateGPUTransferBuffer(device,&ti);check(download);
    GpuWarpInputs inputs{buffers[0],buffers[1],buffers[2],buffers[3],buffers[4],buffers[5],buffers[6],buffers[7],buffers[8],buffers[9],buffers[10],buffers[11]};
    GpuWarpSettings settings{};settings.capacity=4;settings.face_count=1;settings.visibility_count=1;settings.corner_count=3;settings.colour_base=128;settings.flags=1;settings.override_colour=143;
    for(U fixture=0;fixture<8;++fixture){
        traversal={fixture==1?1U:fixture==2?0U:4U,fixture==3?1U:0U};visible=fixture==4?0U:1U;order[1]=fixture==5?1U:0U;
        const void* data[]{order.data(),traversal.data(),polygon.data(),corners.data(),&visible,&material,normal.data(),diffuse.data(),depth.data(),lookup.data(),texture.data(),coordinates.data()};
        auto* mapped=static_cast<unsigned char*>(SDL_MapGPUTransferBuffer(device,upload,true));check(mapped);U offset=0;
        for(U i=0;i<12;++i){std::memcpy(mapped+offset,data[i],sizes[i]);offset+=sizes[i];}SDL_UnmapGPUTransferBuffer(device,upload);
        auto* command=SDL_AcquireGPUCommandBuffer(device);check(command);auto* copy=SDL_BeginGPUCopyPass(command);check(copy);offset=0;
        for(U i=0;i<12;++i){SDL_GPUTransferBufferLocation from{upload,offset};SDL_GPUBufferRegion to{buffers[i],0,sizes[i]};SDL_UploadToGPUBuffer(copy,&from,&to,true);offset+=sizes[i];}SDL_EndGPUCopyPass(copy);
        auto output=warp.enqueue(device,command,inputs,settings);if(!output.polygons)throw std::runtime_error(warp.status());
        if(fixture==6){check(SDL_CancelGPUCommandBuffer(command));continue;}
        copy=SDL_BeginGPUCopyPass(command);check(copy);offset=0;
        void* outputs[]{output.polygons,output.corners,output.materials,output.result};const U lengths[]{64,2048,384,8};
        for(U i=0;i<4;++i){SDL_GPUBufferRegion from{static_cast<SDL_GPUBuffer*>(outputs[i]),0,lengths[i]};SDL_GPUTransferBufferLocation to{download,offset};SDL_DownloadFromGPUBuffer(copy,&from,&to);offset+=lengths[i];}SDL_EndGPUCopyPass(copy);
        auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);check(fence);check(SDL_WaitForGPUFences(device,true,&fence,1));SDL_ReleaseGPUFence(device,fence);
        auto* raw=static_cast<const unsigned char*>(SDL_MapGPUTransferBuffer(device,download,false));check(raw);
        auto* polys=reinterpret_cast<const Four*>(raw);auto* verts=reinterpret_cast<const Four*>(raw+64);auto* mats=reinterpret_cast<const RasterCommand*>(raw+2112);auto* result=reinterpret_cast<const U*>(raw+2496);
        const bool failed=fixture==3 || fixture==5;
        if(result[0]!=(failed?0U:traversal[0]) || result[1]!=U(failed))throw std::runtime_error("chain status mismatch");
        for(U i=0;i<4;++i){bool valid=!failed && visible && i<traversal[0];
            if(!valid){if(polys[i]!=Four{} || mats[i].has_surface)throw std::runtime_error("chain stale output");continue;}
            if(polys[i]!=Four{i*32,3,0,0} || mats[i].even!=143 || mats[i].odd!=143 || mats[i].textured || mats[i].surface!=material.surface)throw std::runtime_error("chain material mismatch");
            for(U c=0;c<3;++c)if(verts[i*32+c]!=corners[c])throw std::runtime_error("chain corner mismatch");
        }SDL_UnmapGPUTransferBuffer(device,download);
    }
    warp.release_device();SDL_ReleaseGPUTransferBuffer(device,upload);SDL_ReleaseGPUTransferBuffer(device,download);for(auto* b:buffers)SDL_ReleaseGPUBuffer(device,b);SDL_DestroyGPUDevice(device);SDL_Quit();
    std::cout<<"Resident warp chain: repeated faces, short/empty/hidden/invalid lists, cancellation and reuse pass\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
