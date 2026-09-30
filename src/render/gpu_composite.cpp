#include "starfox/render/gpu_composite.hpp"
#include <cstring>
#include <stdexcept>
#if defined(STARFOX_SDL_GPU_EFFECTS)
#include <SDL3/SDL.h>
#include "shaders/generated/composite_portable.hpp"
#endif
namespace starfox::render {
#if defined(STARFOX_SDL_GPU_EFFECTS)
namespace {void checked(bool ok) {if(!ok) throw std::runtime_error(SDL_GetError());}}
struct GpuComposite::Impl {
    SDL_GPUDevice* device{};SDL_GPUComputePipeline* pipeline{};
    SDL_GPUBuffer* buffers[8]{};Uint32 capacities[8]{};
    SDL_GPUTransferBuffer *upload{},*download{};Uint32 uploadSize{},downloadSize{};
    SDL_GPUTexture* rgba{};SDL_GPUCommandBuffer* command{};SDL_GPUFence* fence{};
    Uint32 width{},height{};bool valid{},has_depth{},has_motion{};
    std::vector<Uint32> packed;
    std::string status{"GPU composition not initialized"};
    ~Impl() {
        if(!device) return;
        if(command) SDL_CancelGPUCommandBuffer(command);
        if(fence) {SDL_WaitForGPUFences(device,true,&fence,1);SDL_ReleaseGPUFence(device,fence);}
        if(rgba) SDL_ReleaseGPUTexture(device,rgba);
        for(auto* b:buffers) if(b) SDL_ReleaseGPUBuffer(device,b);
        if(upload) SDL_ReleaseGPUTransferBuffer(device,upload);
        if(download) SDL_ReleaseGPUTransferBuffer(device,download);
        if(pipeline) SDL_ReleaseGPUComputePipeline(device,pipeline);
    }
    void finish() {if(fence) {checked(SDL_WaitForGPUFences(device,true,&fence,1));SDL_ReleaseGPUFence(device,fence);fence=nullptr;}}
    void initialize(SDL_GPUDevice* d) {
        device=d;SDL_GPUComputePipelineCreateInfo info{};
        if(SDL_GetGPUShaderFormats(d)&SDL_GPU_SHADERFORMAT_SPIRV) {
            info.format=SDL_GPU_SHADERFORMAT_SPIRV;info.code=composite_shader::spirv;
            info.code_size=sizeof(composite_shader::spirv);info.entrypoint="main";
        } else if(SDL_GetGPUShaderFormats(d)&SDL_GPU_SHADERFORMAT_MSL) {
            info.format=SDL_GPU_SHADERFORMAT_MSL;info.code=reinterpret_cast<const Uint8*>(composite_shader::metal);
            info.code_size=sizeof(composite_shader::metal)-1;info.entrypoint="main0";
        } else if(SDL_GetGPUShaderFormats(d)&SDL_GPU_SHADERFORMAT_DXIL) {
            info.format=SDL_GPU_SHADERFORMAT_DXIL;info.code=composite_shader::dxil;
            info.code_size=sizeof(composite_shader::dxil);info.entrypoint="main";
        } else throw std::runtime_error("GPU composition requires Vulkan, Metal or D3D12");
        info.num_readonly_storage_buffers=8;info.num_readwrite_storage_buffers=5;
        info.num_readwrite_storage_textures=1;info.num_uniform_buffers=1;
        info.threadcount_x=info.threadcount_y=8;info.threadcount_z=1;
        pipeline=SDL_CreateGPUComputePipeline(d,&info);checked(pipeline);
        status=std::string("GPU composition: ")+SDL_GetGPUDeviceDriver(d);
    }
    void buffer(unsigned i,Uint32 bytes) {
        if(buffers[i] && capacities[i]>=bytes) return;
        if(buffers[i]) SDL_ReleaseGPUBuffer(device,buffers[i]);
        buffers[i]=nullptr;
        SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ
            |((i>=2 && i<=3) || i>=5?SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE:0U),bytes,0};
        buffers[i]=SDL_CreateGPUBuffer(device,&info);checked(buffers[i]);capacities[i]=bytes;
    }
    void transfer(SDL_GPUTransferBuffer*& b,Uint32& capacity,Uint32 bytes,SDL_GPUTransferBufferUsage usage) {
        if(b && capacity>=bytes) return;
        if(b) SDL_ReleaseGPUTransferBuffer(device,b);
        b=nullptr;
        SDL_GPUTransferBufferCreateInfo info{usage,bytes,0};
        b=SDL_CreateGPUTransferBuffer(device,&info);checked(b);capacity=bytes;
    }
    void compose(const GpuRasterOutput& source,Uint32 sourceScale,const Framebuffer& cpu,
        std::span<const Uint8> foreground,const LayerCompositeSettings& settings,std::span<const Rgba8> palette,
        const GpuRasterOutput* late,const GpuCompositeBackground* background,std::span<const Uint8> afterLate,bool worldOnly) {
        finish();valid=false;
        const auto count=cpu.pixels().size();
        if(!count || count>UINT32_MAX/24 || palette.empty() || palette.size()>256
            || (!foreground.empty() && foreground.size()!=count)
            || (!afterLate.empty() && afterLate.size()!=count) || !sourceScale
            || !source.width || !source.height || source.width%sourceScale || source.height%sourceScale)
            throw std::runtime_error("Invalid GPU composition inputs");
        if(late && (late->device!=device || !late->pixels || late->width!=cpu.stored_width()
            || late->height!=cpu.stored_height() || late->pixels==buffers[2]
            || late->pixels==buffers[3])) throw std::runtime_error("Invalid late GPU overlay");
        if(background) {
            const auto& b=background->raster;
            if(b.device!=device || !b.pixels || b.width!=cpu.stored_width() || b.height!=cpu.stored_height()
                || b.pixels==buffers[2] || b.pixels==buffers[3]
                || (!background->cpu_coverage.empty() && background->cpu_coverage.size()!=count))
                throw std::runtime_error("Invalid resident GPU background");
            if(background->margin_origin && (!background->margin_width
                || background->margin_origin>=cpu.width()
                || background->margin_width>cpu.width()-background->margin_origin))
                throw std::runtime_error("Invalid GPU background margins");
        }
        const Uint32 bytes=Uint32(count*4);
        if(width!=cpu.stored_width() || height!=cpu.stored_height()) {
            if(rgba) SDL_ReleaseGPUTexture(device,rgba);
            rgba=nullptr;width=height=0;
            SDL_GPUTextureCreateInfo info{};info.type=SDL_GPU_TEXTURETYPE_2D;
            info.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage=SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE|SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width=cpu.stored_width();info.height=cpu.stored_height();info.layer_count_or_depth=1;info.num_levels=1;
            rgba=SDL_CreateGPUTexture(device,&info);checked(rgba);width=info.width;height=info.height;
        }
        buffer(0,bytes);buffer(1,1024);buffer(2,bytes);buffer(3,bytes*4);buffer(4,16);buffer(5,8);
        for(unsigned i=2;i<8;++i)
            if((source.geometry_depth==buffers[i] && source.geometry_depth)
                || (source.motion==buffers[i] && source.motion))
                throw std::runtime_error("Temporal inputs alias compositor output");
        has_depth=source.geometry_depth!=nullptr;has_motion=source.motion!=nullptr;
        buffer(6,has_depth?bytes:4);buffer(7,has_motion?bytes*4:16);
        transfer(upload,uploadSize,bytes+1024,SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
        packed.resize(count);
        for(std::size_t i=0;i<count;++i) packed[i]=cpu.pixels()[i]
            |(cpu.layer_tags_enabled()?Uint32(cpu.layer_tags()[i])<<8:0)
            |(!foreground.empty() && foreground[i]?0x80000000U:0)
            |(background && !background->cpu_coverage.empty() && background->cpu_coverage[i]?0x40000000U:0)
            |(!afterLate.empty() && afterLate[i]?0x20000000U:0);
        auto* mapped=static_cast<Uint8*>(SDL_MapGPUTransferBuffer(device,upload,true));checked(mapped);
        std::memcpy(mapped,packed.data(),bytes);
        for(unsigned i=0;i<256;++i) {
            const auto c=palette[std::min<std::size_t>(i,palette.size()-1)];
            const Uint32 p=c.r|(Uint32(c.g)<<8)|(Uint32(c.b)<<16)|(Uint32(c.a)<<24);
            std::memcpy(mapped+bytes+i*4,&p,4);
        }
        SDL_UnmapGPUTransferBuffer(device,upload);
        command=SDL_AcquireGPUCommandBuffer(device);checked(command);
        auto* copy=SDL_BeginGPUCopyPass(command);checked(copy);
        for(unsigned i=0;i<2;++i) {
            SDL_GPUTransferBufferLocation from{upload,i?bytes:0};SDL_GPUBufferRegion to{buffers[i],0,i?1024U:bytes};
            SDL_UploadToGPUBuffer(copy,&from,&to,false);
        }
        SDL_EndGPUCopyPass(copy);
        Sint32 constants[]{Sint32(width),Sint32(height),Sint32(source.width),Sint32(source.height),
            Sint32(cpu.draw_scale()),Sint32(sourceScale),
            (settings.mosaic & settings.mosaic_layer_mask)?Sint32((settings.mosaic>>4)+1):1,source.surfaces?1:0,
            settings.offset_x,settings.offset_y,settings.clip_left,settings.clip_top,
            settings.clip_right,settings.clip_bottom,settings.mosaic_origin_x,settings.mosaic_origin_y,
            late?1:0,background?1:0,1,background?Sint32(background->margin_origin):0,
            background?Sint32(background->margin_width):0,background && background->repair_transparent_margins?1:0,has_depth?1:0,has_motion?1:0,
            worldOnly?1:0,0,0,0};
        SDL_GPUStorageTextureReadWriteBinding texture{};texture.texture=rgba;
        SDL_GPUStorageBufferReadWriteBinding outputs[5]{};outputs[0].buffer=buffers[2];outputs[1].buffer=buffers[3];outputs[2].buffer=buffers[5];outputs[3].buffer=buffers[6];outputs[4].buffer=buffers[7];
        SDL_GPUBuffer* inputs[]{buffers[0],static_cast<SDL_GPUBuffer*>(source.pixels),
            source.surfaces?static_cast<SDL_GPUBuffer*>(source.surfaces):buffers[4],buffers[1],
            late?static_cast<SDL_GPUBuffer*>(late->pixels):buffers[4],
            background?static_cast<SDL_GPUBuffer*>(background->raster.pixels):buffers[4],
            has_depth?static_cast<SDL_GPUBuffer*>(source.geometry_depth):buffers[4],
            has_motion?static_cast<SDL_GPUBuffer*>(source.motion):buffers[4]};
        for(unsigned phase=constants[19]?0U:1U;phase<2;++phase) {
            constants[18]=Sint32(phase);SDL_PushGPUComputeUniformData(command,0,constants,sizeof(constants));
            auto* pass=SDL_BeginGPUComputePass(command,&texture,1,outputs,5);checked(pass);
            SDL_BindGPUComputePipeline(pass,pipeline);SDL_BindGPUComputeStorageBuffers(pass,0,inputs,8);
            SDL_DispatchGPUCompute(pass,phase?(width+7)/8:1,phase?(height+7)/8:1,1);SDL_EndGPUComputePass(pass);
        }
        fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);command=nullptr;checked(fence);valid=true;
    }
    void read(Framebuffer& target,std::vector<Uint8>& colours,SurfaceBuffer* surfaces) {
        if(!valid || target.stored_width()!=width || target.stored_height()!=height
            || (surfaces && (surfaces->width()!=width || surfaces->height()!=height)))
            throw std::runtime_error("Invalid composition readback dimensions");
        finish();const Uint32 bytes=width*height*4;
        transfer(download,downloadSize,bytes*6,SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
        command=SDL_AcquireGPUCommandBuffer(device);checked(command);
        auto* pass=SDL_BeginGPUCopyPass(command);checked(pass);
        SDL_GPUTextureRegion tex{rgba,0,0,0,0,0,width,height,1};SDL_GPUTextureTransferInfo out{download,0,0,0};
        SDL_DownloadFromGPUTexture(pass,&tex,&out);
        for(unsigned i=0;i<(surfaces?2U:1U);++i) {
            SDL_GPUBufferRegion from{buffers[2+i],0,i?bytes*4:bytes};SDL_GPUTransferBufferLocation to{download,i?bytes*2:bytes};
            SDL_DownloadFromGPUBuffer(pass,&from,&to);
        }
        SDL_EndGPUCopyPass(pass);fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);command=nullptr;checked(fence);finish();
        const auto* mapped=static_cast<const Uint8*>(SDL_MapGPUTransferBuffer(device,download,false));checked(mapped);
        colours.assign(mapped,mapped+bytes);
        const auto* values=reinterpret_cast<const Uint32*>(mapped+bytes);
        const auto* normals=reinterpret_cast<const float*>(mapped+bytes*2);
        if(surfaces) surfaces->clear();
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const auto i=std::size_t(y)*width+x;target.set_stored(x,y,Uint8(values[i]),PixelLayer((values[i]>>8)&255));
            if(surfaces && (values[i]&(1U<<24))) surfaces->set(x,y,
                {normals[i*4],normals[i*4+1],normals[i*4+2],normals[i*4+3]},Uint8(values[i]>>16));
        }
        SDL_UnmapGPUTransferBuffer(device,download);
    }
};
#else
struct GpuComposite::Impl {std::string status{"GPU composition unavailable on this build"};};
#endif
GpuComposite::GpuComposite():impl_(std::make_unique<Impl>()) {}
GpuComposite::~GpuComposite()=default;
void GpuComposite::release_device() noexcept {impl_.reset();}
const std::string& GpuComposite::status() const {static const std::string empty{"GPU composition released"};return impl_?impl_->status:empty;}
GpuCompositeOutput GpuComposite::output() const {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    if(impl_ && impl_->valid) return {impl_->device,impl_->rgba,impl_->buffers[2],impl_->buffers[3],impl_->width,impl_->height,
        impl_->has_depth?impl_->buffers[6]:nullptr,impl_->has_motion?impl_->buffers[7]:nullptr};
#endif
    return {};
}
bool GpuComposite::compose(const GpuRasterOutput& source,std::uint32_t scale,const Framebuffer& cpu,
    std::span<const std::uint8_t> foreground,const LayerCompositeSettings& settings,std::span<const Rgba8> palette,
    const GpuRasterOutput* late,const GpuCompositeBackground* background,std::span<const std::uint8_t> afterLate,bool worldOnly) {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    if(!source.device || !source.pixels) return false;
    if(!impl_ || impl_->device!=source.device) impl_=std::make_unique<Impl>();
    try {if(!impl_->device) impl_->initialize(static_cast<SDL_GPUDevice*>(source.device));impl_->compose(source,scale,cpu,foreground,settings,palette,late,background,afterLate,worldOnly);return true;}
    catch(const std::exception& e) {if(impl_->command) {SDL_CancelGPUCommandBuffer(impl_->command);impl_->command=nullptr;}
        impl_->valid=false;impl_->status=e.what();return false;}
#else
    (void)source;(void)scale;(void)cpu;(void)foreground;(void)settings;(void)palette;(void)late;(void)background;(void)afterLate;(void)worldOnly;return false;
#endif
}
bool GpuComposite::readback(Framebuffer& target,std::vector<std::uint8_t>& rgba,SurfaceBuffer* surfaces) {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    if(!impl_ || !impl_->valid) return false;
    try {impl_->read(target,rgba,surfaces);return true;}
    catch(const std::exception& e) {if(impl_->command) {SDL_CancelGPUCommandBuffer(impl_->command);impl_->command=nullptr;}
        impl_->status=e.what();return false;}
#else
    (void)target;(void)rgba;(void)surfaces;return false;
#endif
}
}
