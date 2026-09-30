#include "starfox/render/gpu_scene.hpp"
#include "starfox/render/gpu_model.hpp"
#include "starfox/render/gpu_projection.hpp"
#include "starfox/render/gpu_ray_geometry.hpp"
#include "starfox/render/dust_renderer.hpp"
#include "starfox/render/stereo_output.hpp"
#include <cstring>
#include <stdexcept>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <iterator>
#if defined(STARFOX_SDL_GPU_EFFECTS)
#include <SDL3/SDL.h>
#include "shaders/generated/scene_portable.hpp"
#endif
namespace starfox::render {
bool enqueue_stereo_texture_pack(void* command,void* left,void* right,
    void* destination,StereoOutput mode,std::uint32_t width,std::uint32_t height) {
    const auto layout=stereo_output_layout(mode,width,height);
    if(!layout || !command || !left || !destination || destination==left
        || (layout->eye_count==2 && (!right || destination==right || left==right))) return false;
#if defined(STARFOX_SDL_GPU_EFFECTS)
    for(unsigned eye=0;eye<layout->eye_count;++eye) {
        SDL_GPUBlitInfo blit{};
        blit.source={static_cast<SDL_GPUTexture*>(eye?right:left),0,0,0,0,width,height};
        blit.destination={static_cast<SDL_GPUTexture*>(destination),0,0,
            layout->eyes[eye].x,0,layout->eyes[eye].width,height};
        // Only the first eye clears. Cycling or clearing the second pass can
        // discard the first eye; retain the same backing texture throughout.
        blit.load_op=eye?SDL_GPU_LOADOP_LOAD:SDL_GPU_LOADOP_CLEAR;
        blit.clear_color={0,0,0,1};
        blit.filter=mode==StereoOutput::half_sbs?SDL_GPU_FILTER_LINEAR:SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(static_cast<SDL_GPUCommandBuffer*>(command),&blit);
    }
    return true;
#else
    return false;
#endif
}
std::optional<std::vector<GpuSceneDraw>> stereo_scene_eye(
    std::span<const GpuSceneDraw> frame,unsigned eye,double separation,double convergence) {
    if(eye>1 || !stereo_eye_projections(separation,convergence,1)) return {};
    std::vector<GpuSceneDraw> result(frame.begin(),frame.end());
    for(auto& draw:result) if(auto* model=std::get_if<GpuModelDraw>(&draw)) {
        const auto offsets=stereo_eye_projections(separation,convergence,model->settings.focal_length);
        if(!offsets) return {};
        model->pose.x-=(*offsets)[eye].eye_x;
        model->pose.vanish_x+=(*offsets)[eye].projection_offset_x;
        if(!std::isfinite(model->pose.x) || !std::isfinite(model->pose.vanish_x)) return {};
        // Integer native projection would round away a subpixel eye offset.
        model->pose.continuous_geometry=true;
        model->pose.subpixel_projection=true;
        if(model->previous_pose) {
            model->previous_pose->x-=(*offsets)[eye].eye_x;
            model->previous_pose->vanish_x+=(*offsets)[eye].projection_offset_x;
            if(!std::isfinite(model->previous_pose->x) || !std::isfinite(model->previous_pose->vanish_x)) return {};
            model->previous_pose->continuous_geometry=true;
            model->previous_pose->subpixel_projection=true;
        }
    } else if(auto* grid=std::get_if<GpuGridDraw>(&draw)) {
        grid->eye_x=float((eye?1:-1)*separation*.5);
        grid->convergence=float(convergence);
    } else if(auto* text=std::get_if<GpuTextDraw>(&draw)) {
        text->eye_x=float((eye?1:-1)*separation*.5);text->convergence=float(convergence);
    } else if(auto* particles=std::get_if<GpuParticleDraw>(&draw)) {
        particles->eye_x=float((eye?1:-1)*separation*.5);particles->convergence=float(convergence);
    } else if(auto* dust=std::get_if<GpuDustDraw>(&draw)) {
        dust->eye_x=float((eye?1:-1)*separation*.5);dust->convergence=float(convergence);
    }
    return result;
}
std::optional<std::array<GpuRasterOutput,2>> GpuStereoScene::enqueue(
    void* device,void* command,std::uint32_t width,std::uint32_t height,
    std::span<const GpuSceneDraw> frame,double separation,double convergence) {
    resident_ready_=false;
    const auto left=stereo_scene_eye(frame,0,separation,convergence);
    const auto right=stereo_scene_eye(frame,1,separation,convergence);
    if(!left || !right || !device || !command || !width || !height) return {};
    std::array<GpuRasterOutput,2> result;
    result[0]=eyes_[0].enqueue_batch(device,command,width,height,*left);
    if(!result[0].pixels) return {};
    result[1]=eyes_[1].enqueue_batch(device,command,width,height,*right);
    if(!result[1].pixels) return {};
    return result;
}
bool GpuStereoScene::render_resident(void* device,std::uint32_t width,std::uint32_t height,
    std::span<const GpuSceneDraw> frame,double separation,double convergence) {
    resident_ready_=false;
    const auto left=stereo_scene_eye(frame,0,separation,convergence);
    const auto right=stereo_scene_eye(frame,1,separation,convergence);
    if(!left || !right || !device || !width || !height) return false;
    // Each scene retains its own submission fences and buffer generations.
    // A failure never exposes a new left eye paired with a stale right eye.
    if(!eyes_[0].render_resident(device,width,height,*left)
        || !eyes_[1].render_resident(device,width,height,*right)) return false;
    resident_ready_=true;return true;
}
void GpuSceneRecording::reset(std::uint32_t width,std::uint32_t height) {
    draws_.clear();raster_.clear();width_=width;height_=height;
}
void GpuSceneRecording::flush(RasterCommands& pending) {
    if(!width_ || !height_ || pending.width()!=width_ || pending.height()!=height_)
        throw std::runtime_error("Invalid recorded scene raster dimensions");
    if(pending.commands.empty()) return;
    raster_.push_back(std::move(pending));
    pending.reset(width_,height_);
    draws_.emplace_back(GpuRasterDraw{&raster_.back(),true,true});
}
void GpuSceneRecording::append_model(RasterCommands& pending,GpuModelDraw draw) {
    const auto scale=draw.settings.render_scale;
    if(!draw.shape || scale<1 || scale>4 || width_%scale || height_%scale)
        throw std::runtime_error("Invalid recorded scene model");
    flush(pending);draws_.emplace_back(std::move(draw));
}
void GpuSceneRecording::finish(RasterCommands& pending) {flush(pending);}
void GpuSceneRecording::append_background(RasterCommands& pending,GpuBackgroundDraw draw) {
    if(!draw.ppu || !draw.scale || draw.scale>4 || width_%draw.scale || height_%draw.scale
        || draw.settings.layer<1 || draw.settings.layer>3)
        throw std::runtime_error("Invalid recorded background");
    flush(pending);draws_.emplace_back(std::move(draw));
}
void GpuSceneRecording::append_text(RasterCommands& pending,GpuTextDraw draw) {
    if(!draw.scale || draw.scale>4 || width_%draw.scale || height_%draw.scale || draw.frame.glyphs.size()>256)
        throw std::runtime_error("Invalid recorded text");
    if(draw.frame.glyphs.empty()) return;
    flush(pending);draws_.emplace_back(std::move(draw));
}
void GpuSceneRecording::append_particles(RasterCommands& pending,GpuParticleDraw draw) {
    if(!draw.scale || draw.scale>4 || width_%draw.scale || height_%draw.scale
        || draw.frame.particles.size()>300)
        throw std::runtime_error("Invalid recorded particles");
    std::erase_if(draw.frame.particles,[&](const auto& p){return !p.life || p.owner!=draw.frame.owner;});
    if(draw.frame.particles.empty()) return;
    flush(pending);draws_.emplace_back(std::move(draw));
}
void GpuSceneRecording::append_dust(RasterCommands& pending,GpuDustDraw draw) {
    if(!draw.scale || draw.scale>4 || width_%draw.scale || height_%draw.scale
        || draw.frame.points.size()>simulation::kMaximumDustPoints)
        throw std::runtime_error("Invalid recorded dust");
    if(draw.frame.points.empty()) return;
    flush(pending);draws_.emplace_back(std::move(draw));
}
void GpuSceneRecording::append_grid(RasterCommands& pending,GpuGridDraw draw) {
    if(!draw.scale || draw.scale>4 || width_%draw.scale || height_%draw.scale)
        throw std::runtime_error("Invalid recorded grid scale");
    flush(pending);draws_.emplace_back(std::move(draw));
}
void GpuSceneRecording::replay(Framebuffer& frame,SurfaceBuffer* surfaces) const {
    if(frame.stored_width()!=width_ || frame.stored_height()!=height_
        || (surfaces && (surfaces->width()!=width_ || surfaces->height()!=height_)))
        throw std::runtime_error("Recorded scene replay dimensions differ");
    frame.record_to(nullptr);frame.clear();if(surfaces) surfaces->clear();
    // Scene initialization is transparent, not a full-screen black draw.
    if(frame.tracks_write_coverage()) frame.begin_write_coverage();
    struct RestoreScale {
        Framebuffer& frame;std::uint32_t scale;
        ~RestoreScale(){frame.set_draw_scale(scale);}
    } restore{frame,frame.draw_scale()};
    for(const auto& draw:draws_) {
        if(const auto* model=std::get_if<GpuModelDraw>(&draw)) {
            frame.set_draw_scale(model->settings.render_scale);
            SoftwareRenderer(model->settings).draw(*model->shape,model->pose,frame,false,
                model->surface_metadata?surfaces:nullptr);
        } else if(const auto* bg=std::get_if<GpuBackgroundDraw>(&draw)) {
            frame.set_draw_scale(bg->scale);const ScopedLayer tag(frame,bg->settings.tag);
            const auto& s=bg->settings;
            if(s.layer==1) BackgroundRenderer{}.draw_bg1(*bg->ppu,frame,s.priority,s.horizontal_origin,
                s.extend_horizontal,s.horizontal_inset,s.transparent_cgram_black);
            else if(s.layer==2) BackgroundRenderer{}.draw_bg2(*bg->ppu,s.scroll_x,s.scroll_y,frame,s.priority,
                s.horizontal_origin,s.extend_horizontal,s.wrap_horizontal,s.transparent_cgram_black,s.single_occurrence_top_rows,s.unique_regions);
            else BackgroundRenderer{}.draw_bg3(*bg->ppu,frame,s.priority,s.horizontal_origin,s.extend_horizontal);
        } else if(const auto* text=std::get_if<GpuTextDraw>(&draw)) {
            frame.set_draw_scale(text->scale);ScaledTextRenderer::draw_projected(text->frame,frame);
        } else if(const auto* particles=std::get_if<GpuParticleDraw>(&draw)) {
            frame.set_draw_scale(particles->scale);ParticleRenderer::draw_frame(particles->frame,frame);
        } else if(const auto* dust=std::get_if<GpuDustDraw>(&draw)) {
            frame.set_draw_scale(dust->scale);DustRenderer::draw_dust_frame(dust->frame,frame);
        } else if(const auto* grid=std::get_if<GpuGridDraw>(&draw)) {
            frame.set_draw_scale(grid->scale);
            const ScopedLayer layer{frame,PixelLayer::world_geometry};
            const auto points=project_source_grid(grid->camera,grid->matrix,width_/grid->scale,height_/grid->scale);
            if(grid->lines) {
                DustRenderer::draw_grid_lines_frame({points,grid->line_start},frame,grid->colour);
                continue;
            }
            for(std::size_t i=0;i<points.count;++i) {
                const auto& p=points.points[i];
                frame.set(p.x,p.y,grid->colour);
                if(p.depth<512) frame.set(p.x-1,p.y+1,grid->colour);
            }
        } else {
            const auto& raster=std::get<GpuRasterDraw>(draw);
            replay_raster_commands(*raster.commands,frame,raster.surface_metadata?surfaces:nullptr,false);
        }
    }
}
struct GpuScene::Impl {
    std::string status{"GPU scene merge unavailable"};
    GpuModel models[2];
    GpuRaster raster;
    GpuBackground background;
    GpuProjection grid_projection;
    GpuRaster grid_raster;
    GpuRayGeometry ray_expander;
#if defined(STARFOX_SDL_GPU_EFFECTS)
    SDL_GPUDevice* device{};SDL_GPUComputePipeline* pipeline{};
    SDL_GPUFence* fence{};
    std::vector<SDL_GPUFence*> retired;
    bool owned_encoding{};
    GpuRasterOutput resident{};
    SDL_GPUBuffer* pixels[2]{},*surfaces[2]{};Uint32 capacity[2]{};std::uint64_t generation{};
    SDL_GPUBuffer* depths[2]{};Uint32 depth_capacity[2]{};
    SDL_GPUBuffer* motions[2]{};Uint32 motion_capacity[2]{};
    SDL_GPUBuffer* ray_vertices{};
    struct RayTopology {
        std::vector<std::array<std::uint32_t,4>> triangles;
        SDL_GPUBuffer* buffer{};
        bool submitted{};
    };
    std::vector<RayTopology> ray_topologies;
    std::size_t ray_topology_bytes{};
    SDL_GPUTransferBuffer* ray_upload{};
    Uint32 ray_capacity{},topology_capacity{},ray_vertex_count{};
    RayGeometryOutput ray_output{};
    ~Impl(){release();}
    static void require(bool value){if(!value) throw std::runtime_error(SDL_GetError());}
    void release()noexcept {
        if(fence) {
            SDL_WaitForGPUFences(device,true,&fence,1);
            SDL_ReleaseGPUFence(device,fence);fence=nullptr;
        }
        if(!retired.empty()) SDL_WaitForGPUFences(device,true,retired.data(),Uint32(retired.size()));
        for(auto* previous:retired) SDL_ReleaseGPUFence(device,previous);
        retired.clear();
        resident={};
        ray_output={};ray_vertex_count=0;
        ray_expander.release_device();
        if(ray_vertices) SDL_ReleaseGPUBuffer(device,ray_vertices);
        clear_ray_topologies(true);
        if(ray_upload) SDL_ReleaseGPUTransferBuffer(device,ray_upload);
        ray_vertices=nullptr;ray_upload=nullptr;ray_capacity=topology_capacity=0;
        for(auto& model:models) model.release_device();
        raster.release_device();background.release_device();
        grid_projection.release_device();grid_raster.release_device();
        for(unsigned i=0;i<2;++i){if(pixels[i]) SDL_ReleaseGPUBuffer(device,pixels[i]);if(surfaces[i]) SDL_ReleaseGPUBuffer(device,surfaces[i]);pixels[i]=surfaces[i]=nullptr;capacity[i]=0;}
        for(unsigned i=0;i<2;++i){if(depths[i]) SDL_ReleaseGPUBuffer(device,depths[i]);depths[i]=nullptr;depth_capacity[i]=0;}
        for(unsigned i=0;i<2;++i){if(motions[i]) SDL_ReleaseGPUBuffer(device,motions[i]);motions[i]=nullptr;motion_capacity[i]=0;}
        if(pipeline) SDL_ReleaseGPUComputePipeline(device,pipeline);
        pipeline=nullptr;device=nullptr;
    }
    bool pending() const noexcept {return fence || !retired.empty();}
    void clear_ray_topologies(bool all) {
        for(auto it=ray_topologies.begin();it!=ray_topologies.end();) {
            if(all || !it->submitted) {
                if(it->buffer) SDL_ReleaseGPUBuffer(device,it->buffer);
                ray_topology_bytes-=it->triangles.size()*16U;
                it=ray_topologies.erase(it);
            } else ++it;
        }
    }
    void prepare_rays(Uint32 triangles) {
        const auto bytes=triangles*48U;
        if(ray_capacity>=bytes) return;
        SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,bytes,0};
        auto* buffer=SDL_CreateGPUBuffer(device,&info);require(buffer);
        if(ray_vertices) SDL_ReleaseGPUBuffer(device,ray_vertices);
        ray_vertices=buffer;ray_capacity=bytes;
    }
    void append_rays(SDL_GPUCommandBuffer* command,const GpuModelRaySource& source) {
        if(source.triangles.empty()) return;
        const auto bytes=source.triangles.size()*16U;
        if(bytes>UINT32_MAX || (std::uint64_t(ray_vertex_count)+source.triangles.size()*3U)*16U>ray_capacity)
            throw std::runtime_error("GPU ray scene exceeds reserved topology");
        auto found=std::find_if(ray_topologies.begin(),ray_topologies.end(),[&](const auto& item){return item.triangles==source.triangles;});
        if(found==ray_topologies.end()) {
            if(ray_topologies.size()>=128 || ray_topology_bytes+bytes>16U*1024*1024) clear_ray_topologies(true);
            ray_topologies.push_back({source.triangles,nullptr,false});ray_topology_bytes+=bytes;
            found=std::prev(ray_topologies.end());
            SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,Uint32(bytes),0};
            found->buffer=SDL_CreateGPUBuffer(device,&info);require(found->buffer);
            if(topology_capacity<bytes) {
            SDL_GPUTransferBufferCreateInfo upload_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,Uint32(bytes),0};
            auto* upload=SDL_CreateGPUTransferBuffer(device,&upload_info);require(upload);
            if(ray_upload) SDL_ReleaseGPUTransferBuffer(device,ray_upload);
            ray_upload=upload;
            topology_capacity=Uint32(bytes);
            }
        auto* mapped=SDL_MapGPUTransferBuffer(device,ray_upload,true);require(mapped);
        std::memcpy(mapped,source.triangles.data(),bytes);SDL_UnmapGPUTransferBuffer(device,ray_upload);
        auto* copy=SDL_BeginGPUCopyPass(command);require(copy);
        SDL_GPUTransferBufferLocation from{ray_upload,0};SDL_GPUBufferRegion to{found->buffer,0,Uint32(bytes)};
        SDL_UploadToGPUBuffer(copy,&from,&to,false);SDL_EndGPUCopyPass(copy);
        }
        GpuRayGeometrySettings settings;settings.triangles=Uint32(source.triangles.size());
        settings.points=source.point_count;settings.mode=source.mode;
        const GpuRayGeometryTarget target{ray_vertices,ray_capacity/16U,ray_vertex_count,ray_vertex_count==0};
        auto* expanded=static_cast<SDL_GPUBuffer*>(ray_expander.enqueue(device,command,source.points,source.residuals,found->buffer,settings,&target));
        if(!expanded) throw std::runtime_error(ray_expander.status());
        // Write the reserved scene range directly, avoiding a temporary output
        // and an extra GPU copy/barrier pair for every caster model.
        ray_vertex_count+=Uint32(source.triangles.size()*3U);
    }
    void finish() {
        if(fence) {
            require(SDL_WaitForGPUFences(device,true,&fence,1));
            SDL_ReleaseGPUFence(device,fence);fence=nullptr;
        }
        if(!retired.empty()) require(SDL_WaitForGPUFences(device,true,retired.data(),Uint32(retired.size())));
        for(auto* previous:retired) SDL_ReleaseGPUFence(device,previous);
        retired.clear();
    }
    void retire_submission() {
        if(fence) {retired.push_back(fence);fence=nullptr;}
        // Cycle-enabled buffers retain earlier commands' backing storage.
        // Bound outstanding work to two older scenes plus the next submission.
        while(!retired.empty() && (retired.size()>2 || SDL_QueryGPUFence(device,retired.front()))) {
            auto* previous=retired.front();require(SDL_WaitForGPUFences(device,true,&previous,1));
            SDL_ReleaseGPUFence(device,previous);retired.erase(retired.begin());
        }
    }
    void initialize(SDL_GPUDevice* next) {
        if(device==next && pipeline) return;
        release();device=next;
        bool spirv=(SDL_GetGPUShaderFormats(device)&SDL_GPU_SHADERFORMAT_SPIRV)!=0;
        const bool dxil=(SDL_GetGPUShaderFormats(device)&SDL_GPU_SHADERFORMAT_DXIL)!=0;
        if(!spirv && !dxil && !(SDL_GetGPUShaderFormats(device)&SDL_GPU_SHADERFORMAT_MSL)) throw std::runtime_error("GPU scene merge requires Vulkan, Metal or D3D12");
        SDL_GPUComputePipelineCreateInfo info{};
        info.format=spirv?SDL_GPU_SHADERFORMAT_SPIRV:dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_MSL;
        info.code=spirv?scene_shader::spirv:dxil?scene_shader::dxil:reinterpret_cast<const Uint8*>(scene_shader::metal);
        info.code_size=spirv?sizeof(scene_shader::spirv):dxil?sizeof(scene_shader::dxil):std::strlen(scene_shader::metal);info.entrypoint=(spirv||dxil)?"main":"main0";
        info.num_uniform_buffers=1;info.num_readonly_storage_buffers=8;info.num_readwrite_storage_buffers=4;
        info.threadcount_x=64;info.threadcount_y=info.threadcount_z=1;
        pipeline=SDL_CreateGPUComputePipeline(device,&info);require(pipeline);
    }
    void allocate(unsigned slot,Uint32 count,bool depth,bool motion) {
        const auto motion_count=motion?count:1U;
        if(motion_capacity[slot]<motion_count) {
            SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,motion_count*16,0};
            auto* m=SDL_CreateGPUBuffer(device,&info);require(m);
            if(motions[slot]) SDL_ReleaseGPUBuffer(device,motions[slot]);
            motions[slot]=m;motion_capacity[slot]=motion_count;
        }
        const auto depth_count=depth?count:1U;
        if(depth_capacity[slot]<depth_count) {
            SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,depth_count*4,0};
            auto* d=SDL_CreateGPUBuffer(device,&info);require(d);
            if(depths[slot]) SDL_ReleaseGPUBuffer(device,depths[slot]);
            depths[slot]=d;depth_capacity[slot]=depth_count;
        }
        if(capacity[slot]>=count) return;
        SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,count*4,0};
        auto* p=SDL_CreateGPUBuffer(device,&info);require(p);info.size=count*16;
        auto* s=SDL_CreateGPUBuffer(device,&info);if(!s){SDL_ReleaseGPUBuffer(device,p);require(false);}
        if(pixels[slot]) SDL_ReleaseGPUBuffer(device,pixels[slot]);
        if(surfaces[slot]) SDL_ReleaseGPUBuffer(device,surfaces[slot]);
        pixels[slot]=p;surfaces[slot]=s;capacity[slot]=count;
    }
#endif
};
GpuScene::GpuScene():impl_(std::make_unique<Impl>()){}
GpuScene::~GpuScene()=default;
const std::string& GpuScene::status()const noexcept{return impl_->status;}
void GpuScene::release_device()noexcept {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    impl_->release();
#endif
}
GpuRasterOutput GpuScene::enqueue(void* command,const GpuRasterOutput& front,const GpuRasterOutput* back) {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    try {
        if(!impl_->owned_encoding && impl_->pending()) throw std::runtime_error("Finish submitted scene work before borrowed enqueue");
        impl_->resident={};
        auto count=std::uint64_t(front.width)*front.height;
        if(!command || !front.device || !front.pixels || !count || count>UINT32_MAX/16
            || (back && (!back->pixels || back->device!=front.device || back->width!=front.width || back->height!=front.height)))
            throw std::runtime_error("Invalid GPU scene merge input");
        impl_->initialize(static_cast<SDL_GPUDevice*>(front.device));
        unsigned slot=0;
        const auto aliases=[&](unsigned index){
            const auto matches=[&](void* p){return p && (p==impl_->pixels[index] || p==impl_->surfaces[index] || p==impl_->depths[index] || p==impl_->motions[index]);};
            return matches(front.pixels) || matches(front.surfaces) || matches(front.geometry_depth) || matches(front.motion)
                || (back && (matches(back->pixels) || matches(back->surfaces) || matches(back->geometry_depth) || matches(back->motion)));
        };
        // Null handles are not aliases of unallocated scratch.
        if(impl_->pixels[0] && aliases(0)) slot=1;
        if(impl_->pixels[slot] && aliases(slot)) throw std::runtime_error("GPU scene merge has no non-aliased scratch slot");
        const bool depth=front.geometry_depth || (back && back->geometry_depth);
        const bool motion=front.motion || (back && back->motion);
        impl_->allocate(slot,Uint32(count),depth,motion);
        auto* cmd=static_cast<SDL_GPUCommandBuffer*>(command);
        const Uint32 settings[]{Uint32(count),front.surfaces?1U:0U,back?1U:0U,back && back->surfaces?1U:0U,
            depth?1U:0U,front.geometry_depth?1U:0U,back && back->geometry_depth?1U:0U,0,
            motion?1U:0U,front.motion?1U:0U,back && back->motion?1U:0U,0};
        SDL_PushGPUComputeUniformData(cmd,0,settings,sizeof(settings));
        SDL_GPUStorageBufferReadWriteBinding outputs[4]{};outputs[0].buffer=impl_->pixels[slot];outputs[1].buffer=impl_->surfaces[slot];outputs[2].buffer=impl_->depths[slot];outputs[3].buffer=impl_->motions[slot];
        outputs[0].cycle=outputs[1].cycle=outputs[2].cycle=outputs[3].cycle=true;
        auto* pass=SDL_BeginGPUComputePass(cmd,nullptr,0,outputs,4);Impl::require(pass);SDL_BindGPUComputePipeline(pass,impl_->pipeline);
        SDL_GPUBuffer* inputs[]{static_cast<SDL_GPUBuffer*>(front.pixels),static_cast<SDL_GPUBuffer*>(front.surfaces?front.surfaces:front.pixels),
            static_cast<SDL_GPUBuffer*>(back?back->pixels:front.pixels),static_cast<SDL_GPUBuffer*>(back && back->surfaces?back->surfaces:front.pixels),
            static_cast<SDL_GPUBuffer*>(front.geometry_depth?front.geometry_depth:front.pixels),
            static_cast<SDL_GPUBuffer*>(back && back->geometry_depth?back->geometry_depth:front.pixels),
            static_cast<SDL_GPUBuffer*>(front.motion?front.motion:front.pixels),
            static_cast<SDL_GPUBuffer*>(back && back->motion?back->motion:front.pixels)};
        SDL_BindGPUComputeStorageBuffers(pass,0,inputs,8);SDL_DispatchGPUCompute(pass,(Uint32(count)+63)/64,1,1);SDL_EndGPUComputePass(pass);
        impl_->status="GPU scene painter merge resident";
        return {front.device,impl_->pixels[slot],impl_->surfaces[slot],front.width,front.height,++impl_->generation,
            depth?impl_->depths[slot]:nullptr,motion?impl_->motions[slot]:nullptr};
    }catch(const std::exception& error){impl_->status=error.what();}
#endif
    return {};
}
GpuRasterOutput GpuScene::enqueue_batch(void* device,void* command,std::uint32_t width,
    std::uint32_t height,std::span<const GpuSceneDraw> draws) {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    try {
        if(!impl_->owned_encoding && impl_->pending()) throw std::runtime_error("Finish submitted scene work before borrowed batch enqueue");
        impl_->resident={};
        impl_->ray_output={};impl_->ray_vertex_count=0;
        if(!device || !command || !width || !height || std::uint64_t(width)*height>UINT32_MAX/16)
            throw std::runtime_error("Invalid GPU scene batch dimensions");
        // Validate the entire layout before encoding any draws. Geometry packing
        // can still fail later; the caller must cancel, never submit a partial scene.
        std::uint64_t ray_triangles=0;bool rays_requested=false;
        bool rays_complete=std::getenv("STARFOX_DISABLE_GPU_RAY_GEOMETRY")==nullptr;
        for(const auto& draw:draws) {
            if(const auto* model=std::get_if<GpuModelDraw>(&draw)) {
                const auto scale=model->settings.render_scale;
                if(!model->shape || scale<1 || scale>4 || width%scale || height%scale
                    || width/scale>32767 || height/scale>32767)
                    throw std::runtime_error("Invalid GPU scene model layout");
                if(model->ray_geometry) {
                    rays_requested=true;
                    const auto& pose=model->pose;
                    // Full-scene fallback: don't spend GPU time expanding the
                    // ordinary casters when another caster needs a different
                    // producer. Never publish an incomplete caster set.
                    if(pose.simple_scaled_sprite) {
                        if(std::getenv("STARFOX_TRACE_GPU_RAYS")) std::cerr<<"ray-scene unsupported pose: "<<model->shape->name<<'\n';
                        rays_complete=false;
                    }
                    for(const auto& face:model->shape->faces)
                        if(!face.sprite && face.vertex_indices.size()>=3) ray_triangles+=face.vertex_indices.size()-2;
                    if(ray_triangles>1'000'000) throw std::runtime_error("GPU ray scene topology too large");
                }
            } else if(const auto* bg=std::get_if<GpuBackgroundDraw>(&draw)) {
                if(!bg->ppu || !bg->scale || bg->scale>4 || width%bg->scale || height%bg->scale
                    || bg->settings.layer<1 || bg->settings.layer>3)
                    throw std::runtime_error("Invalid GPU background layout");
            } else if(const auto* text=std::get_if<GpuTextDraw>(&draw)) {
                if(!text->scale || text->scale>4 || width%text->scale || height%text->scale || text->frame.glyphs.size()>256)
                    throw std::runtime_error("Invalid GPU text layout");
            } else if(const auto* particles=std::get_if<GpuParticleDraw>(&draw)) {
                if(!particles->scale || particles->scale>4 || width%particles->scale || height%particles->scale
                    || particles->frame.particles.size()>300)
                    throw std::runtime_error("Invalid GPU particle layout");
            } else if(const auto* dust=std::get_if<GpuDustDraw>(&draw)) {
                if(!dust->scale || dust->scale>4 || width%dust->scale || height%dust->scale
                    || dust->frame.points.size()>simulation::kMaximumDustPoints
                    || std::abs(std::int64_t(dust->frame.exclude_left))>32767
                    || std::abs(std::int64_t(dust->frame.exclude_right))>32767)
                    throw std::runtime_error("Invalid GPU dust layout");
            } else if(const auto* grid=std::get_if<GpuGridDraw>(&draw)) {
                if(!grid->scale || grid->scale>4 || width%grid->scale || height%grid->scale)
                    throw std::runtime_error("Invalid GPU grid layout");
            } else {
                const auto& raster=std::get<GpuRasterDraw>(draw);
                if(!raster.commands || raster.commands->width()!=width || raster.commands->height()!=height)
                    throw std::runtime_error("Invalid GPU scene raster layout");
            }
        }
        // Initialize before encoding layers: switching devices must not release
        // the first layer's newly allocated model/raster resources during merge.
        impl_->initialize(static_cast<SDL_GPUDevice*>(device));
        // Borrowed/failed commands can be cancelled by their owner. Only
        // successful owned submissions promote uploads to persistent cache.
        impl_->clear_ray_topologies(false);
        const bool encode_rays=rays_complete && rays_requested;
        if(encode_rays && ray_triangles) impl_->prepare_rays(Uint32(ray_triangles));
        if(draws.empty()) {
            RasterCommands empty;empty.reset(width,height);
            auto output=impl_->raster.enqueue_commands(device,command,empty,true);
            if(!output.pixels) throw std::runtime_error(impl_->raster.status());
            impl_->status="Empty GPU scene batch cleared resident";
            return output;
        }
        GpuRasterOutput output{};
        unsigned model_slot=0;
        const bool fused=!std::getenv("STARFOX_TEST_SEPARATE_SCENE_MERGE");
        for(const auto& draw:draws) {
            GpuRasterOutput front{};
            if(const auto* model=std::get_if<GpuModelDraw>(&draw)) {
                const auto scale=model->settings.render_scale;
                auto& renderer=impl_->models[model_slot];model_slot^=1U;
                const bool fuse_model=fused && !model->previous_pose && !output.motion;
                GpuModelRaySource rays;
                front=renderer.enqueue(device,command,*model->shape,model->pose,model->settings,
                    width/scale,height/scale,model->surface_metadata,fuse_model && output.pixels?&output:nullptr,nullptr,model->geometry_depth,
                    encode_rays && model->ray_geometry?&rays:nullptr,model->previous_pose?&*model->previous_pose:nullptr,model->raster_jitter);
                if(!front.pixels) throw std::runtime_error(renderer.status());
                if(encode_rays && model->ray_geometry) {
                    if(!rays.points) {
                        if(std::getenv("STARFOX_TRACE_GPU_RAYS")) std::cerr<<"ray-scene missing producer: "<<model->shape->name<<" status="<<renderer.status()<<'\n';
                        rays_complete=false;
                    }
                    else impl_->append_rays(static_cast<SDL_GPUCommandBuffer*>(command),rays);
                }
                if(fuse_model) {output=front;continue;}
            } else if(const auto* bg=std::get_if<GpuBackgroundDraw>(&draw)) {
                front=impl_->background.enqueue(device,command,*bg->ppu,width,height,bg->scale,bg->settings);
                if(!front.pixels) throw std::runtime_error(impl_->background.status());
            } else if(const auto* text=std::get_if<GpuTextDraw>(&draw)) {
                auto& projection=impl_->grid_projection;
                auto* pixels=projection.enqueue_text(device,command,text->frame,width,height,text->scale,
                    static_cast<std::uint8_t>(PixelLayer::three_d),text->eye_x,text->convergence);
                if(!pixels) throw std::runtime_error(projection.status());
                front={device,pixels,nullptr,width,height};
            } else if(const auto* particles=std::get_if<GpuParticleDraw>(&draw)) {
                const auto& frame=particles->frame;
                const auto count=std::count_if(frame.particles.begin(),frame.particles.end(),
                    [&](const auto& p){return p.life && p.owner==frame.owner;});
                if(!count) continue;
                auto& projection=impl_->grid_projection;
                if(!projection.enqueue_particle_frame(device,command,frame,width/particles->scale,height/particles->scale,
                    particles->eye_x,particles->convergence)) throw std::runtime_error(projection.status());
                auto* spans=projection.enqueue_particle_spans(command,height,particles->scale,
                    static_cast<std::uint8_t>(PixelLayer::three_d),frame.pose.effect_clip_left,frame.pose.effect_clip_right);
                if(!spans) throw std::runtime_error(projection.status());
                front=impl_->grid_raster.enqueue_row_spans(device,command,spans,std::uint32_t(count),width,height,false,nullptr,true);
                if(!front.pixels) throw std::runtime_error(impl_->grid_raster.status());
            } else if(const auto* dust=std::get_if<GpuDustDraw>(&draw)) {
                if(dust->frame.points.empty()) continue;
                auto& projection=impl_->grid_projection;
                if(!projection.enqueue_dust_frame(device,command,dust->frame,width/dust->scale,height/dust->scale,dust->eye_x,dust->convergence))
                    throw std::runtime_error(projection.status());
                auto* spans=projection.enqueue_dust_spans(command,height,dust->scale,
                    static_cast<std::uint8_t>(PixelLayer::world_geometry),std::int16_t(dust->frame.exclude_left),std::int16_t(dust->frame.exclude_right));
                if(!spans) throw std::runtime_error(projection.status());
                front=impl_->grid_raster.enqueue_row_spans(device,command,spans,std::uint32_t(dust->frame.points.size()),width,height,false,nullptr,true);
                if(!front.pixels) throw std::runtime_error(impl_->grid_raster.status());
            } else if(const auto* grid=std::get_if<GpuGridDraw>(&draw)) {
                auto& projection=impl_->grid_projection;
                if(!projection.enqueue_grid(device,command,source_grid_lattice(grid->camera,grid->matrix),
                    width/grid->scale,height/grid->scale,grid->eye_x,grid->convergence))
                    throw std::runtime_error(projection.status());
                auto* spans=projection.enqueue_grid_spans(command,height,grid->scale,grid->colour,
                    static_cast<std::uint8_t>(PixelLayer::world_geometry),grid->lines?grid->line_start.data():nullptr);
                if(!spans) throw std::runtime_error(projection.status());
                front=impl_->grid_raster.enqueue_row_spans(device,command,spans,grid->lines?675:225,width,height,false,nullptr,true);
                if(!front.pixels) throw std::runtime_error(impl_->grid_raster.status());
            } else {
                const auto& raster=std::get<GpuRasterDraw>(draw);
                front=impl_->raster.enqueue_commands(device,command,*raster.commands,
                    raster.surface_metadata,raster.gpu_binning);
                if(!front.pixels) throw std::runtime_error(impl_->raster.status());
            }
            const auto back=output;
            output=enqueue(command,front,back.pixels?&back:nullptr);
            if(!output.pixels) throw std::runtime_error(impl_->status);
        }
        if(!output.pixels) {
            // A nonempty list may consist entirely of inactive particle/dust draws.
            // It must clear just like an empty list, not retain the previous frame.
            RasterCommands empty;empty.reset(width,height);
            output=impl_->raster.enqueue_commands(device,command,empty,true);
            if(!output.pixels) throw std::runtime_error(impl_->raster.status());
        }
        impl_->status="Ordered mixed GPU scene batch resident";
        if(rays_requested && rays_complete)
            impl_->ray_output={device,impl_->ray_vertex_count?impl_->ray_vertices:nullptr,impl_->ray_vertex_count,true};
        return output;
    } catch(const std::exception& error) {impl_->ray_output={};impl_->status=error.what();}
#endif
    return {};
}
bool GpuScene::render_resident(void* device,std::uint32_t width,std::uint32_t height,
    std::span<const GpuSceneDraw> draws) {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    SDL_GPUCommandBuffer* command=nullptr;
    try {
        const bool profile=std::getenv("STARFOX_TRACE_SCENE_COST")!=nullptr;
        const auto begin=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        if(std::getenv("STARFOX_TEST_SERIAL_SCENE")) impl_->finish();
        else impl_->retire_submission();
        const auto retired=profile?std::chrono::steady_clock::now():begin;
        impl_->resident={};
        if(!device) throw std::runtime_error("Missing GPU scene device");
        command=SDL_AcquireGPUCommandBuffer(static_cast<SDL_GPUDevice*>(device));Impl::require(command);
        impl_->owned_encoding=true;
        const auto output=enqueue_batch(device,command,width,height,draws);
        const auto encoded=profile?std::chrono::steady_clock::now():begin;
        impl_->owned_encoding=false;
        if(!output.pixels) throw std::runtime_error(impl_->status);
        auto* submitted=command;command=nullptr;
        impl_->fence=SDL_SubmitGPUCommandBufferAndAcquireFence(submitted);Impl::require(impl_->fence);
        for(auto& topology:impl_->ray_topologies) topology.submitted=true;
        if(profile) {
            const auto submitted_at=std::chrono::steady_clock::now();
            const auto us=[](auto a,auto b){return std::chrono::duration_cast<std::chrono::microseconds>(b-a).count();};
            std::cerr<<"scene-cost-us retire="<<us(begin,retired)<<" encode="<<us(retired,encoded)
                <<" submit="<<us(encoded,submitted_at)<<" draws="<<draws.size()<<'\n';
        }
        impl_->resident=output;
        impl_->status="Ordered GPU scene submitted resident";
        return true;
    } catch(const std::exception& error) {
        impl_->owned_encoding=false;
        if(command) SDL_CancelGPUCommandBuffer(command);
        impl_->resident={};impl_->ray_output={};impl_->status=error.what();
    }
#else
    (void)device;(void)width;(void)height;(void)draws;
#endif
    return false;
}
GpuRasterOutput GpuScene::resident_output() const noexcept {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    return impl_->resident;
#else
    return {};
#endif
}
GpuScene::RayGeometryOutput GpuScene::ray_geometry_output() const noexcept {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    return impl_->ray_output;
#else
    return {};
#endif
}
bool GpuScene::wait_for_completion() {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    if(!impl_->resident.pixels && !impl_->pending()) return false;
    try {impl_->finish();}
    catch(const std::exception& error){impl_->status=error.what();return false;}
    return true;
#else
    return false;
#endif
}
bool GpuScene::readback(Framebuffer& frame,SurfaceBuffer* surfaces) {
#if defined(STARFOX_SDL_GPU_EFFECTS)
    const auto output=resident_output();
    if(!output.pixels || frame.stored_width()!=output.width || frame.stored_height()!=output.height
        || (surfaces && (surfaces->width()!=output.width || surfaces->height()!=output.height))) return false;
    auto* device=static_cast<SDL_GPUDevice*>(output.device);
    SDL_GPUCommandBuffer* command=nullptr;
    SDL_GPUTransferBuffer* transfer=nullptr;
    SDL_GPUFence* fence=nullptr;
    try {
        if(!wait_for_completion()) return false;
        const Uint32 pixels=output.width*output.height,bytes=pixels*4;
        const bool normals=surfaces && output.surfaces;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;info.size=bytes+(normals?pixels*16:0);
        transfer=SDL_CreateGPUTransferBuffer(device,&info);Impl::require(transfer);
        command=SDL_AcquireGPUCommandBuffer(device);Impl::require(command);
        auto* copy=SDL_BeginGPUCopyPass(command);Impl::require(copy);
        for(unsigned i=0;i<(normals?2U:1U);++i) {
            SDL_GPUBufferRegion source{static_cast<SDL_GPUBuffer*>(i?output.surfaces:output.pixels),0,i?pixels*16:bytes};
            SDL_GPUTransferBufferLocation target{transfer,i?bytes:0};
            SDL_DownloadFromGPUBuffer(copy,&source,&target);
        }
        SDL_EndGPUCopyPass(copy);
        auto* submitted=command;command=nullptr;
        fence=SDL_SubmitGPUCommandBufferAndAcquireFence(submitted);Impl::require(fence);
        Impl::require(SDL_WaitForGPUFences(device,true,&fence,1));
        const bool coverage=frame.tracks_write_coverage();
        if(coverage) frame.begin_write_coverage();
        const auto* packed=static_cast<const Uint32*>(SDL_MapGPUTransferBuffer(device,transfer,false));Impl::require(packed);
        const auto* values=reinterpret_cast<const float*>(packed+pixels);
        if(surfaces) surfaces->clear();
        for(unsigned y=0;y<output.height;++y) for(unsigned x=0;x<output.width;++x) {
            const auto i=std::size_t(y)*output.width+x;
            if(coverage && (packed[i]&(1U<<26))) frame.set_stored(x,y,std::uint8_t(packed[i]));
            else frame.pixels()[i]=std::uint8_t(packed[i]);
            if(frame.layer_tags_enabled()) frame.layer_tags()[i]=std::uint8_t(packed[i]>>8);
            if(normals && (packed[i]&(1U<<24))) surfaces->set(x,y,
                {values[i*4],values[i*4+1],values[i*4+2],values[i*4+3]},std::uint8_t(packed[i]>>16));
        }
        SDL_UnmapGPUTransferBuffer(device,transfer);
        SDL_ReleaseGPUFence(device,fence);SDL_ReleaseGPUTransferBuffer(device,transfer);
        return true;
    } catch(const std::exception& error) {
        if(command) SDL_CancelGPUCommandBuffer(command);
        if(fence) SDL_ReleaseGPUFence(device,fence);
        if(transfer) SDL_ReleaseGPUTransferBuffer(device,transfer);
        impl_->status=error.what();
    }
#else
    (void)frame;(void)surfaces;
#endif
    return false;
}
}
