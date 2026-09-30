#include "starfox/render/gpu_ray_geometry.hpp"
#include "starfox/render/gpu_model.hpp"
#include "starfox/render/gpu_scene.hpp"
#include "starfox/render/sdl_dxr_shadows.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
void require(bool value,const char* what) {if(!value) throw std::runtime_error(std::string(what)+": "+SDL_GetError());}
using Point=std::array<std::uint32_t,8>;
using Triangle=std::array<std::uint32_t,4>;
using Position=std::array<float,4>;
std::vector<Position> run(SDL_GPUDevice* device,starfox::render::GpuRayGeometry& geometry,
    const std::vector<Point>& points,const std::vector<Point>& residuals,
    const std::vector<Triangle>& triangles,starfox::render::GpuRayGeometrySettings settings) {
    settings.points=std::uint32_t(points.size());settings.triangles=std::uint32_t(triangles.size());
    const std::array<std::uint32_t,3> sizes{settings.points*32,settings.points*32,settings.triangles*16};
    std::array<SDL_GPUBuffer*,3> inputs{};
    std::uint32_t total=0;
    for(unsigned i=0;i<3;++i) {
        SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,sizes[i],0};
        inputs[i]=SDL_CreateGPUBuffer(device,&info);require(inputs[i],"input buffer");total+=sizes[i];
    }
    SDL_GPUTransferBufferCreateInfo up_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,total,0};
    auto* upload=SDL_CreateGPUTransferBuffer(device,&up_info);require(upload,"upload");
    auto* mapped=static_cast<unsigned char*>(SDL_MapGPUTransferBuffer(device,upload,false));require(mapped,"map upload");
    const void* data[]{points.data(),residuals.data(),triangles.data()};
    std::uint32_t offset=0;for(unsigned i=0;i<3;++i) {std::memcpy(mapped+offset,data[i],sizes[i]);offset+=sizes[i];}
    SDL_UnmapGPUTransferBuffer(device,upload);
    auto* command=SDL_AcquireGPUCommandBuffer(device);require(command,"command");
    auto* copy=SDL_BeginGPUCopyPass(command);offset=0;
    for(unsigned i=0;i<3;++i) {
        SDL_GPUTransferBufferLocation from{upload,offset};SDL_GPUBufferRegion to{inputs[i],0,sizes[i]};
        SDL_UploadToGPUBuffer(copy,&from,&to,false);offset+=sizes[i];
    }
    SDL_EndGPUCopyPass(copy);
    auto* output=static_cast<SDL_GPUBuffer*>(geometry.enqueue(device,command,inputs[0],inputs[1],inputs[2],settings));
    if(!output) {SDL_CancelGPUCommandBuffer(command);throw std::runtime_error(geometry.status());}
    auto bad=settings;bad.row0[0]=std::numeric_limits<float>::infinity();
    require(!geometry.enqueue(device,command,inputs[0],inputs[1],inputs[2],bad),"invalid transform accepted");
    require(!geometry.enqueue(device,command,output,inputs[1],inputs[2],settings),"output alias accepted");
    bad=settings;bad.mode=3;
    require(!geometry.enqueue(device,command,inputs[0],inputs[1],inputs[2],bad),"unknown mode accepted");
    const starfox::render::GpuRayGeometryTarget overflow{output,settings.triangles*3U,1,false};
    require(!geometry.enqueue(device,command,inputs[0],inputs[1],inputs[2],settings,&overflow),"external target overflow accepted");
    const starfox::render::GpuRayGeometryTarget alias{inputs[0],settings.triangles*3U,0,false};
    require(!geometry.enqueue(device,command,inputs[0],inputs[1],inputs[2],settings,&alias),"external target aliases source");
    SDL_GPUTransferBufferCreateInfo down_info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,settings.triangles*48,0};
    auto* download=SDL_CreateGPUTransferBuffer(device,&down_info);require(download,"download");
    copy=SDL_BeginGPUCopyPass(command);
    SDL_GPUBufferRegion from{output,0,down_info.size};SDL_GPUTransferBufferLocation to{download,0};
    SDL_DownloadFromGPUBuffer(copy,&from,&to);SDL_EndGPUCopyPass(copy);
    auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);require(fence,"submit");
    require(SDL_WaitForGPUFences(device,true,&fence,1),"wait");SDL_ReleaseGPUFence(device,fence);
    std::vector<Position> result(settings.triangles*3);
    mapped=static_cast<unsigned char*>(SDL_MapGPUTransferBuffer(device,download,false));require(mapped,"map download");
    std::memcpy(result.data(),mapped,down_info.size);SDL_UnmapGPUTransferBuffer(device,download);
    for(auto* input:inputs) SDL_ReleaseGPUBuffer(device,input);
    SDL_ReleaseGPUTransferBuffer(device,upload);SDL_ReleaseGPUTransferBuffer(device,download);
    return result;
}
void check_model(SDL_GPUDevice* device,starfox::render::GpuRayGeometry& geometry,bool continuous,unsigned effect=0) {
    starfox::render::GpuModel model;
    starfox::assets::Shape shape;shape.vertices={{80,60,-360},{-80,60,-440},{0,-70,-400}};
    shape.word_coordinates=std::vector<bool>(3,true);
    starfox::assets::Face face;face.visibility_index=-1;face.normal={0,0,127};face.vertex_indices={0,2,1};
    shape.faces={face};shape.colour_words={0x3f11};shape.colour_materials={{0x3f11,{}}};
    starfox::render::RenderPose pose;pose.z=0;pose.use_rotation_matrix=true;
    pose.rotation_matrix={-32768,0,0,0,-32768,0,0,0,-32768};
    pose.continuous_geometry=continuous;pose.subpixel_projection=continuous;pose.vanish_x=112;pose.vanish_y=96;
    pose.wave_mode=effect==1;pose.wave_offset=11;
    pose.wobble_mode=effect>=2 && effect<=4?effect-1:0;
    pose.colour_warp=effect==5 || effect==7 || effect==9;
    pose.collapse_to_axis_line=effect>=6;
    pose.explosion_progress=effect>=8?1:0;
    auto* command=SDL_AcquireGPUCommandBuffer(device);require(command,"model command");
    starfox::render::GpuModelRaySource source;
    auto raster=model.enqueue(device,command,shape,pose,{},224,192,false,nullptr,nullptr,false,&source);
    require(raster.pixels && source.points && source.triangles.size()==1,"model ray source unavailable");
    SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,16,0};
    auto* topology=SDL_CreateGPUBuffer(device,&info);require(topology,"topology");
    SDL_GPUTransferBufferCreateInfo up_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,16,0};
    auto* upload=SDL_CreateGPUTransferBuffer(device,&up_info);require(upload,"topology upload");
    auto* bytes=SDL_MapGPUTransferBuffer(device,upload,false);require(bytes,"topology map");
    std::memcpy(bytes,source.triangles.data(),16);SDL_UnmapGPUTransferBuffer(device,upload);
    auto* copy=SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation from{upload,0};SDL_GPUBufferRegion to{topology,0,16};
    SDL_UploadToGPUBuffer(copy,&from,&to,false);SDL_EndGPUCopyPass(copy);
    starfox::render::GpuRayGeometrySettings settings;settings.triangles=1;settings.points=source.point_count;settings.mode=source.mode;
    auto* expanded=static_cast<SDL_GPUBuffer*>(geometry.enqueue(device,command,source.points,source.residuals,topology,settings));require(expanded,"model expansion");
    SDL_GPUTransferBufferCreateInfo down_info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,48,0};
    auto* download=SDL_CreateGPUTransferBuffer(device,&down_info);require(download,"model download");
    copy=SDL_BeginGPUCopyPass(command);SDL_GPUBufferRegion gpu{expanded,0,48};SDL_GPUTransferBufferLocation cpu{download,0};
    SDL_DownloadFromGPUBuffer(copy,&gpu,&cpu);SDL_EndGPUCopyPass(copy);
    auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);require(fence,"model submit");
    require(SDL_WaitForGPUFences(device,true,&fence,1),"model wait");SDL_ReleaseGPUFence(device,fence);
    const auto* result=static_cast<const Position*>(SDL_MapGPUTransferBuffer(device,download,false));require(result,"model result");
    std::array<Position,3> expected{{{-80,-60,360,1},{0,70,400,1},{80,-60,440,1}}};
    if(effect>=8) for(auto& point:expected) point[2]+=31;
    for(unsigned c=0;c<3;++c) require(result[c]==expected[c],"model producer/expander mismatch");
    SDL_UnmapGPUTransferBuffer(device,download);
    SDL_ReleaseGPUTransferBuffer(device,download);SDL_ReleaseGPUTransferBuffer(device,upload);SDL_ReleaseGPUBuffer(device,topology);
}
void check_scene(SDL_GPUDevice* device) {
    using namespace starfox::render;
    GpuScene scene;
#if defined(_WIN32)
    shadows::SdlDxrShadows resident_shadows;
    shadows::DxrShadows reference_shadows;
#endif
    starfox::assets::Shape shape;shape.vertices={{80,60,-360},{-80,60,-440},{0,-70,-400}};
    shape.word_coordinates=std::vector<bool>(3,true);
    starfox::assets::Face face;face.visibility_index=-1;face.normal={0,0,127};face.vertex_indices={0,2,1};
    shape.faces={face};shape.colour_words={0x3f11};shape.colour_materials={{0x3f11,{}}};
    for(unsigned model_count:{1U,33U,2U}) {
        std::vector<GpuSceneDraw> draws;std::vector<Position> expected;
        for(unsigned i=0;i<model_count;++i) {
            GpuModelDraw draw;draw.shape=&shape;draw.pose.z=0;draw.pose.x=i*13;
            draw.pose.use_rotation_matrix=true;draw.pose.rotation_matrix={-32768,0,0,0,-32768,0,0,0,-32768};
            draw.pose.continuous_geometry=(i%2)!=0;draw.pose.subpixel_projection=(i%2)!=0;
            draw.pose.vanish_x=112;draw.pose.vanish_y=96;draw.ray_geometry=i!=1;
            draws.push_back(draw);
            if(draw.ray_geometry) for(const auto point:std::array<Position,3>{{{-80,-60,360,1},{0,70,400,1},{80,-60,440,1}}}) {
                auto p=point;p[0]+=float(i*13);expected.push_back(p);
            }
        }
        auto* command=SDL_AcquireGPUCommandBuffer(device);require(command,"scene command");
        require(scene.enqueue_batch(device,command,224,192,draws).pixels,"scene raster");
        const auto rays=scene.ray_geometry_output();
        require(rays.complete && rays.buffer && rays.vertex_count==expected.size(),"scene caster count/completeness");
        SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,rays.vertex_count*16,0};
        auto* download=SDL_CreateGPUTransferBuffer(device,&info);require(download,"scene download");
        auto* copy=SDL_BeginGPUCopyPass(command);SDL_GPUBufferRegion from{static_cast<SDL_GPUBuffer*>(rays.buffer),0,info.size};
        SDL_GPUTransferBufferLocation to{download,0};SDL_DownloadFromGPUBuffer(copy,&from,&to);SDL_EndGPUCopyPass(copy);
        auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);require(fence,"scene submit");
        require(SDL_WaitForGPUFences(device,true,&fence,1),"scene wait");SDL_ReleaseGPUFence(device,fence);
        const auto* points=static_cast<const Position*>(SDL_MapGPUTransferBuffer(device,download,false));require(points,"scene map");
        for(unsigned i=0;i<expected.size();++i) require(points[i]==expected[i],"scene caster overwritten or reordered");
        SDL_UnmapGPUTransferBuffer(device,download);SDL_ReleaseGPUTransferBuffer(device,download);
#if defined(_WIN32)
        shadows::Scene reference;
        const auto vertex=[](const Position& p){return shadows::Vec3{p[0],p[1],p[2]};};
        for(unsigned i=0;i<expected.size();i+=3)
            reference.add({vertex(expected[i]),vertex(expected[i+1]),vertex(expected[i+2])});
        const shadows::Camera camera{224,192,256,112,96};
        const shadows::Vec3 light{-1,-1,-1};
        const shadows::ReceiverPlane ground{{0,100,0},{0,1,0}};
        std::vector<std::uint8_t> actual_mask,expected_mask;
        require(reference_shadows.render(reference,camera,light,ground,expected_mask),reference_shadows.status().c_str());
        require(resident_shadows.render_resident(device,{},camera,light,ground,&rays),resident_shadows.status().c_str());
        require(resident_shadows.readback(actual_mask),resident_shadows.status().c_str());
        require(actual_mask==expected_mask,"GPU scene -> shared geometry -> DXR mask mismatch");
        auto alternate_shape=shape;alternate_shape.faces[0].vertex_indices={0,1,2};
        auto cancelled_draws=draws;std::get<GpuModelDraw>(cancelled_draws[0]).shape=&alternate_shape;
        command=SDL_AcquireGPUCommandBuffer(device);require(command,"cancelled topology command");
        require(scene.enqueue_batch(device,command,224,192,cancelled_draws).pixels,"cancelled topology encode");
        SDL_CancelGPUCommandBuffer(command);
        require(scene.render_resident(device,224,192,cancelled_draws),scene.status().c_str());
        auto reuploaded=scene.ray_geometry_output();
        require(resident_shadows.render_resident(device,{},camera,light,ground,&reuploaded),resident_shadows.status().c_str());
        require(resident_shadows.readback(actual_mask) && actual_mask==expected_mask,"cancelled topology upload reused");
        require(scene.wait_for_completion(),"reuploaded scene completion");
        // Exercise owned submissions too: do not CPU-wait the scene before
        // its consumer copies the resident geometry on the same SDL queue.
        require(scene.render_resident(device,224,192,draws),scene.status().c_str());
        auto owned=scene.ray_geometry_output();
        require(resident_shadows.render_resident(device,{},camera,light,ground,&owned),resident_shadows.status().c_str());
        require(resident_shadows.readback(actual_mask) && actual_mask==expected_mask,"owned caster submission mismatch");
        require(scene.wait_for_completion(),"owned scene completion");
        if(model_count==2) {
            GpuStereoScene stereo;
            require(stereo.render_resident(device,224,192,draws,6.4,512),"stereo caster submission");
            for(unsigned eye=0;eye<2;++eye) {
                shadows::Scene eye_reference;
                const double eye_x=eye?3.2:-3.2;
                const auto eye_vertex=[&](const Position& p){return shadows::Vec3{float(double(p[0])-eye_x),p[1],p[2]};};
                for(unsigned i=0;i<expected.size();i+=3)
                    eye_reference.add({eye_vertex(expected[i]),eye_vertex(expected[i+1]),eye_vertex(expected[i+2])});
                auto eye_camera=camera;eye_camera.center_x+=camera.focal_length*eye_x/512.;
                require(reference_shadows.render(eye_reference,eye_camera,light,ground,expected_mask),"stereo reference");
                auto eye_rays=stereo.ray_geometry_output(eye);
                require(resident_shadows.render_resident(device,{},eye_camera,light,ground,&eye_rays),resident_shadows.status().c_str());
                require(resident_shadows.readback(actual_mask) && actual_mask==expected_mask,"stereo caster eye mismatch");
            }
        }
#endif
        // One unsupported caster must invalidate the aggregate, never silently
        // export the other models as a supposedly complete shadow scene.
        std::get<GpuModelDraw>(draws[0]).pose.simple_scaled_sprite=true;
        command=SDL_AcquireGPUCommandBuffer(device);require(command,"unsupported scene command");
        require(scene.enqueue_batch(device,command,224,192,draws).pixels,"unsupported caster raster fallback");
        require(!scene.ray_geometry_output().complete && !scene.ray_geometry_output().buffer,"partial caster scene escaped");
        SDL_CancelGPUCommandBuffer(command);
        command=SDL_AcquireGPUCommandBuffer(device);require(command,"empty scene command");
        require(scene.enqueue_batch(device,command,224,192,{}).pixels,"empty scene");
        require(!scene.ray_geometry_output().buffer && !scene.ray_geometry_output().vertex_count,"empty scene retained old casters");
        SDL_CancelGPUCommandBuffer(command);
    }
}
}
int main() try {
    require(SDL_Init(SDL_INIT_VIDEO),"SDL");
    auto props=SDL_CreateProperties();
    SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,true);
    SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN,true);
    SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN,true);
    SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,true);
#if defined(_WIN32)
    starfox::render::shadows::SdlDxrShadows::request_vulkan_interop(props);
#endif
    auto* device=SDL_CreateGPUDeviceWithProperties(props);SDL_DestroyProperties(props);require(device,"device");
    starfox::render::GpuRayGeometry geometry;
    check_model(device,geometry,false);check_model(device,geometry,true);
    for(unsigned effect=1;effect<=9;++effect) {
        check_model(device,geometry,false,effect);check_model(device,geometry,true,effect);
    }
    check_scene(device);
    std::size_t checked=0;
    for(unsigned mode:{0U,1U,2U}) for(unsigned count:{1U,65U,257U,2U}) {
        std::vector<Point> points(4),tails(4);
        std::array<Position,4> expected{};
        for(unsigned i=0;i<4;++i) {
            const float xyz[]{float(int(i)*11-20),float(int(i)*7-9),float(int(i)*19-30)};
            for(unsigned a=0;a<3;++a) {
                float value=xyz[a];
                points[i][a]=mode?std::bit_cast<std::uint32_t>(value):std::bit_cast<std::uint32_t>(int(value));
                if(mode==2 && i%2==0) {tails[i][a]=std::bit_cast<std::uint32_t>(.25F);value+=.25F;}
                if(mode==2 && i%2==1) {
                    const auto bits=std::bit_cast<std::uint64_t>(double(value)+.125);
                    tails[i][a]=std::uint32_t(bits);tails[i][a+4]=std::uint32_t(bits>>32);value+=.125F;
                }
                expected[i][a]=value;
            }
            points[i][3]=mode?std::bit_cast<std::uint32_t>(1.F):0;
            tails[i][3]=std::bit_cast<std::uint32_t>(i%2==1?3.F:1.F);
            expected[i][0]+=17;expected[i][1]-=3;expected[i][2]+=10;expected[i][3]=1;
        }
        std::vector<Triangle> triangles(count);
        for(unsigned i=0;i<count;++i) triangles[i]={i%4,(i+1)%4,(i+2)%4,i};
        starfox::render::GpuRayGeometrySettings settings;settings.mode=mode;
        settings.row0[3]=17;settings.row1[3]=-3;settings.row2[3]=10;
        auto result=run(device,geometry,points,tails,triangles,settings);
        for(unsigned i=0;i<count;++i) for(unsigned c=0;c<3;++c) {require(result[i*3+c]==expected[triangles[i][c]],"expanded point mismatch");++checked;}
        triangles[0][1]=4;result=run(device,geometry,points,tails,triangles,settings);
        for(unsigned c=0;c<3;++c) require(result[c]==Position{},"invalid triangle retained stale positions");
        triangles[0]={0,1,2,0};points[0][3]=mode?std::bit_cast<std::uint32_t>(-1.F):1U;
        result=run(device,geometry,points,tails,triangles,settings);
        for(unsigned c=0;c<3;++c) require(result[c]==Position{},"invalid source point retained stale positions");
    }
    geometry.release_device();
    check_model(device,geometry,true);
    geometry.release_device();
    SDL_DestroyGPUDevice(device);SDL_Quit();
    std::cout<<"GPU ray expansion: "<<checked<<" native/fractional/compensated vertices, negative/offscreen coordinates, resize/reuse and invalidation passed\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
