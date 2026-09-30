#include <SDL3/SDL.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
void require(bool ok) {if(!ok)throw std::runtime_error(SDL_GetError());}
int main(int argc,char** argv) try {
    if(argc!=2)throw std::runtime_error("Pass the compiled geometry_fp64_compile SPIR-V path");
    require(SDL_Init(SDL_INIT_VIDEO));
    auto* device=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,true,nullptr);require(device);
    size_t code_size=0;auto* code=SDL_LoadFile(argv[1],&code_size);require(code);
    SDL_GPUComputePipelineCreateInfo pi{};pi.code=static_cast<Uint8*>(code);pi.code_size=code_size;
    pi.format=SDL_GPU_SHADERFORMAT_SPIRV;pi.entrypoint="main";pi.num_readonly_storage_buffers=1;
    pi.num_readwrite_storage_buffers=1;pi.threadcount_x=64;pi.threadcount_y=pi.threadcount_z=1;
    auto* pipeline=SDL_CreateGPUComputePipeline(device,&pi);SDL_free(code);require(pipeline);
    constexpr Uint32 count=65536,input_bytes=count*16,output_bytes=count*32;
    SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,input_bytes,0};
    auto* input=SDL_CreateGPUBuffer(device,&bi);require(input);
    bi.usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;bi.size=output_bytes;
    auto* output=SDL_CreateGPUBuffer(device,&bi);require(output);
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,input_bytes,0};
    auto* upload=SDL_CreateGPUTransferBuffer(device,&ti);require(upload);
    ti.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;ti.size=output_bytes;
    auto* download=SDL_CreateGPUTransferBuffer(device,&ti);require(download);
    Uint32 state=8197;
    auto random=[&]() {state=state*1664525U+1013904223U;return state;};
    std::vector<std::array<double,2>> pairs(count);
    for(auto& pair:pairs)for(auto& value:pair) {
        const Uint32 hi=(random()&0x800fffffU)|((700+random()%600)<<20);
        value=std::bit_cast<double>((Uint64(hi)<<32)|random());
    }
    pairs[0]={0.,-0.};pairs[1]={-0.,1.};pairs[2]={1.,0.};pairs[3]={1.,-1.};
    auto* mapped=SDL_MapGPUTransferBuffer(device,upload,false);require(mapped);
    std::memcpy(mapped,pairs.data(),input_bytes);SDL_UnmapGPUTransferBuffer(device,upload);
    auto* command=SDL_AcquireGPUCommandBuffer(device);require(command);
    auto* copy=SDL_BeginGPUCopyPass(command);require(copy);
    SDL_GPUTransferBufferLocation from{upload,0};SDL_GPUBufferRegion to{input,0,input_bytes};
    SDL_UploadToGPUBuffer(copy,&from,&to,false);SDL_EndGPUCopyPass(copy);
    SDL_GPUStorageBufferReadWriteBinding binding{output,false};
    auto* pass=SDL_BeginGPUComputePass(command,nullptr,0,&binding,1);require(pass);
    SDL_BindGPUComputePipeline(pass,pipeline);SDL_BindGPUComputeStorageBuffers(pass,0,&input,1);
    SDL_DispatchGPUCompute(pass,count/64,1,1);SDL_EndGPUComputePass(pass);
    copy=SDL_BeginGPUCopyPass(command);require(copy);
    SDL_GPUBufferRegion source{output,0,output_bytes};SDL_GPUTransferBufferLocation destination{download,0};
    SDL_DownloadFromGPUBuffer(copy,&source,&destination);SDL_EndGPUCopyPass(copy);
    auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);require(fence);
    require(SDL_WaitForGPUFences(device,true,&fence,1));SDL_ReleaseGPUFence(device,fence);
    auto* actual=static_cast<const Uint64*>(SDL_MapGPUTransferBuffer(device,download,false));require(actual);
    for(Uint32 i=0;i<count;++i) {
        auto [a,b]=pairs[i];volatile double sum=a+b,difference=a-b,product=a*b,quotient=a/b;
        const std::array<double,4> reference{sum,difference,product,quotient};
        for(unsigned op=0;op<4;++op) {
            const auto value=reference[op];
            const Uint64 expected=std::isfinite(value)&&(value==0.||std::isnormal(value))
                ?std::bit_cast<Uint64>(value):0x7ff8000000000000ULL;
            if(actual[i*4+op]!=expected)throw std::runtime_error("GPU arithmetic mismatch at pair "+std::to_string(i)+" operation "+std::to_string(op));
        }
    }
    SDL_UnmapGPUTransferBuffer(device,download);
    SDL_ReleaseGPUTransferBuffer(device,upload);SDL_ReleaseGPUTransferBuffer(device,download);
    SDL_ReleaseGPUBuffer(device,input);SDL_ReleaseGPUBuffer(device,output);
    SDL_ReleaseGPUComputePipeline(device,pipeline);SDL_DestroyGPUDevice(device);SDL_Quit();
    std::cout<<count*4<<" GPU arithmetic results match native double bits\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
