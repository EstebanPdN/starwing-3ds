#include "starfox/render/gpu_ray_geometry.hpp"
#include <cmath>
#if defined(STARFOX_SDL_GPU_EFFECTS)
#include <SDL3/SDL.h>
#include "shaders/generated/ray_geometry_portable.hpp"
#include <cstring>
#include <stdexcept>
#endif
namespace starfox::render {
struct GpuRayGeometry::Impl {
    std::string status{"GPU ray geometry unavailable"};
#if defined(STARFOX_SDL_GPU_EFFECTS)
    SDL_GPUDevice* device{};SDL_GPUComputePipeline* pipeline{};
    SDL_GPUBuffer* output{};std::uint32_t capacity{};
    static void require(bool ok) {if(!ok) throw std::runtime_error(SDL_GetError());}
    ~Impl() {release();}
    void release() noexcept {
        if(output) SDL_ReleaseGPUBuffer(device,output);
        if(pipeline) SDL_ReleaseGPUComputePipeline(device,pipeline);
        output=nullptr;pipeline=nullptr;device=nullptr;capacity=0;
    }
    void initialize(SDL_GPUDevice* next) {
        if(device==next && pipeline) return;
        release();device=next;
        const auto formats=SDL_GetGPUShaderFormats(device);
        const bool spv=(formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0,dxil=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0;
        if(!spv && !dxil && !(formats&SDL_GPU_SHADERFORMAT_MSL)) throw std::runtime_error("No supported ray geometry shader format");
        SDL_GPUComputePipelineCreateInfo info{};
        info.format=spv?SDL_GPU_SHADERFORMAT_SPIRV:dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_MSL;
        info.code=spv?ray_geometry_shader::spirv:dxil?ray_geometry_shader::dxil:reinterpret_cast<const Uint8*>(ray_geometry_shader::metal);
        info.code_size=spv?sizeof(ray_geometry_shader::spirv):dxil?sizeof(ray_geometry_shader::dxil):std::strlen(ray_geometry_shader::metal);
        info.entrypoint=(spv||dxil)?"main":"main0";
        info.num_readonly_storage_buffers=3;info.num_readwrite_storage_buffers=1;info.num_uniform_buffers=1;
        info.threadcount_x=64;info.threadcount_y=info.threadcount_z=1;
        pipeline=SDL_CreateGPUComputePipeline(device,&info);require(pipeline);
    }
#endif
};
GpuRayGeometry::GpuRayGeometry():impl_(std::make_unique<Impl>()){}
GpuRayGeometry::~GpuRayGeometry()=default;
const std::string& GpuRayGeometry::status()const noexcept {return impl_->status;}
void GpuRayGeometry::release_device()noexcept {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    impl_->release();
#endif
}
void* GpuRayGeometry::enqueue(void* device,void* command,void* points,void* residuals,
    void* triangles,const GpuRayGeometrySettings& settings,const GpuRayGeometryTarget* target) {
    bool valid=device && command && points && triangles && settings.triangles>0
        && settings.triangles<=1'000'000 && settings.points>0 && settings.points<=1'000'000
        && settings.mode<=2 && (settings.mode!=2 || residuals);
    for(const auto& row:{settings.row0,settings.row1,settings.row2}) for(float v:row) valid&=std::isfinite(v);
    if(target) valid&=target->buffer && target->vertex_capacity<=UINT32_MAX/16U
        && std::uint64_t(target->first_vertex)+std::uint64_t(settings.triangles)*3U<=target->vertex_capacity
        && target->buffer!=points && target->buffer!=residuals && target->buffer!=triangles;
    if(!valid) {impl_->status="Invalid GPU ray geometry input";return nullptr;}
#if defined(STARFOX_SDL_GPU_EFFECTS)
    try {
        for(auto* input:{points,residuals,triangles}) if(input && input==impl_->output)
            throw std::runtime_error("Ray geometry input aliases output");
        impl_->initialize(static_cast<SDL_GPUDevice*>(device));
        const auto bytes=settings.triangles*48U;
        if(!target && impl_->capacity<bytes) {
            SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,bytes,0};
            auto* buffer=SDL_CreateGPUBuffer(impl_->device,&info);Impl::require(buffer);
            if(impl_->output) SDL_ReleaseGPUBuffer(impl_->device,impl_->output);
            impl_->output=buffer;impl_->capacity=bytes;
        }
        auto* cmd=static_cast<SDL_GPUCommandBuffer*>(command);
        auto dispatch=settings;dispatch.reserved=target?target->first_vertex:0;
        SDL_PushGPUComputeUniformData(cmd,0,&dispatch,sizeof(dispatch));
        SDL_GPUStorageBufferReadWriteBinding output{};
        output.buffer=target?static_cast<SDL_GPUBuffer*>(target->buffer):impl_->output;
        output.cycle=target?target->cycle:true;
        auto* pass=SDL_BeginGPUComputePass(cmd,nullptr,0,&output,1);Impl::require(pass);
        SDL_BindGPUComputePipeline(pass,impl_->pipeline);
        SDL_GPUBuffer* input[]{static_cast<SDL_GPUBuffer*>(points),static_cast<SDL_GPUBuffer*>(residuals?residuals:points),static_cast<SDL_GPUBuffer*>(triangles)};
        SDL_BindGPUComputeStorageBuffers(pass,0,input,3);
        SDL_DispatchGPUCompute(pass,(settings.triangles+63)/64,1,1);SDL_EndGPUComputePass(pass);
        impl_->status="Ray triangle expansion GPU resident";return output.buffer;
    } catch(const std::exception& error) {impl_->status=error.what();}
#endif
    return nullptr;
}
}
