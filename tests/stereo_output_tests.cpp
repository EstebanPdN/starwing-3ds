#include "starfox/render/stereo_output.hpp"
#include "starfox/render/gpu_scene.hpp"
#include "starfox/render/model_motion_history.hpp"
#include "starfox/render/dust_renderer.hpp"
#include "starfox/render/particle_renderer.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition) { if (!condition) { std::cerr << "Stereo output regression\n"; std::exit(1); } }
}
int main() {
    using namespace starfox::render;
    {
        starfox::assets::Shape shape;
        GpuModelDraw draw{&shape};
        draw.identity=GpuModelIdentity{42,3,7,11,2};
        draw.pose.x=12;
        ModelMotionHistory history;
        const std::array<GpuSceneDraw,1> frame{draw};
        history.commit(frame,{10,1,800,448});
        auto next=draw;next.pose.x=16;
        const auto* old=history.previous(next,{11,1,800,448});
        require(old && old->x==12);
        require(!history.previous(next,{12,1,800,448})); // skipped presentation
        require(!history.previous(next,{11,2,800,448})); // scene/save-state epoch
        require(!history.previous(next,{11,1,1600,448}));
        ++next.identity->generation;
        require(!history.previous(next,{11,1,800,448}));
        next=draw;++next.pose.animation_frame;
        require(history.previous(next,{11,1,800,448})); // static geometry ignores global tick
        starfox::assets::Shape animated;animated.frames.resize(2);
        auto animated_draw=draw;animated_draw.shape=&animated;
        ModelMotionHistory animated_history;
        const std::array<GpuSceneDraw,1> animated_frame{animated_draw};
        animated_history.commit(animated_frame,{10,1,800,448});
        animated_draw.pose.animation_frame=1;
        require(!animated_history.previous(animated_draw,{11,1,800,448}));
        animated_draw.pose.animation_frame=2;
        require(animated_history.previous(animated_draw,{11,1,800,448})); // same wrapped geometry
        next=draw;++next.pose.explosion_progress;
        require(!history.previous(next,{11,1,800,448}));
        next=draw;next.settings.focal_length=128;
        require(!history.previous(next,{11,1,800,448}));
        const std::array<GpuSceneDraw,2> duplicate{draw,draw};
        const auto prepared=history.prepare(frame,{11,1,800,448});
        require(std::get<GpuModelDraw>(prepared[0]).previous_pose.has_value());
        const auto ambiguous=history.prepare(duplicate,{11,1,800,448});
        require(!std::get<GpuModelDraw>(ambiguous[0]).previous_pose);
        require(!std::get<GpuModelDraw>(ambiguous[1]).previous_pose);
        require(history.previous(draw,{11,1,800,448}));
        history.commit(duplicate,{11,1,800,448});
        require(!history.previous(draw,{12,1,800,448}));
        history.commit(frame,{12,1,800,448});
        history.commit({}, {13,1,800,448});
        require(!history.previous(draw,{14,1,800,448})); // disappeared
        history.commit(frame,{14,1,800,448});
        history.reset();
        require(!history.previous(draw,{15,1,800,448}));
    }
    {
        ScaledTextRenderer::ProjectedFrame text;
        text.pose.z=256;text.character_size=16;text.colour=114;
        std::array<std::uint16_t,16> glyph{};glyph[0]=0x8000;glyph[15]=1;
        text.glyphs.push_back(glyph);
        for(unsigned scale:{1U,2U,4U}) {
            Framebuffer pixels(224*scale,192*scale);pixels.set_draw_scale(scale);
            ScaledTextRenderer::draw_projected(text,pixels);
            require(pixels.draw_scale()==scale);
            require(pixels.pixels()[(88*scale)*(224*scale)+104*scale]==114);
            require(pixels.pixels()[(103*scale)*(224*scale)+119*scale]==114);
            require(pixels.pixels()[(96*scale)*(224*scale)+112*scale]==0);
        }
    }
    {
        ParticleRenderer::OwnerFrame frame;
        frame.pose.z=0;
        frame.alpha=.5;
        starfox::simulation::ParticleState trail;
        trail.life=1;trail.flags=4;trail.colour=2;trail.z=trail.previous_z=512;
        trail.previous_x=-10;
        frame.particles.push_back(trail);
        Framebuffer pixels(224,192);
        ParticleRenderer::draw_frame(frame,pixels);
        for(unsigned x=107;x<=110;++x) require(pixels.pixels()[96*224+x]==114);
        require(pixels.pixels()[96*224+111]==0);
        frame.particles[0].flags=0;frame.particles[0].previous_x=0;
        pixels.clear();ParticleRenderer::draw_frame(frame,pixels);
        require(pixels.pixels()[96*224+112]==114 && pixels.pixels()[97*224+113]==114);
        frame.pose.effect_clip_left=113;frame.pose.effect_clip_right=114;
        pixels.clear();ParticleRenderer::draw_frame(frame,pixels);
        require(pixels.pixels()[96*224+112]==0 && pixels.pixels()[96*224+113]==114);
        GpuSceneRecording recording;recording.reset(448,384);
        RasterCommands pending;pending.reset(448,384);
        auto inactive=trail;inactive.life=0;frame.particles.push_back(inactive);
        recording.append_particles(pending,{frame,2});recording.finish(pending);
        frame.particles.clear();
        require(recording.draws().size()==1);
        const auto& stored=std::get<GpuParticleDraw>(recording.draws()[0]);
        require(stored.frame.particles.size()==1);
        Framebuffer replay(448,384),expected(448,384);expected.set_draw_scale(2);
        ParticleRenderer::draw_frame(stored.frame,expected);recording.replay(replay,nullptr);
        require(replay.pixels()==expected.pixels() && replay.draw_scale()==1);
        const auto left=stereo_scene_eye(recording.draws(),0,6.4,512);
        const auto right=stereo_scene_eye(recording.draws(),1,6.4,512);
        require(left && right && std::get<GpuParticleDraw>((*left)[0]).eye_x<0
            && std::get<GpuParticleDraw>((*right)[0]).eye_x>0 && stored.eye_x==0);
        recording.reset(448,384);recording.append_particles(pending,{frame,2});
        require(recording.draws().empty());
    }
    {
        GpuDustDraw dust;
        dust.frame.points={{0,0,512},{10,10,2048}};
        dust.frame.matrix={32767,0,0,0,32767,0,0,0,32767};
        dust.scale=2;
        GpuSceneRecording recording;recording.reset(448,384);
        RasterCommands pending;pending.reset(448,384);
        recording.append_dust(pending,dust);recording.finish(pending);
        dust.frame.points.clear();
        Framebuffer frame(448,384);recording.replay(frame,nullptr);
        require(frame.pixels()[192*448+224]==112);
        const auto left=stereo_scene_eye(recording.draws(),0,6.4,512);
        const auto right=stereo_scene_eye(recording.draws(),1,6.4,512);
        require(left && right);
        require(std::get<GpuDustDraw>((*left)[0]).eye_x<0 && std::get<GpuDustDraw>((*right)[0]).eye_x>0);
        require(std::get<GpuDustDraw>((*left)[0]).frame.points.size()==2);
        require(std::get<GpuDustDraw>(recording.draws()[0]).eye_x==0);
    }
    {
        GpuGridDraw grid;
        grid.camera.y=512;grid.matrix={32767,0,0,0,32767,0,0,0,32767};
        grid.scale=2;
        const std::array<GpuSceneDraw,1> source{grid};
        const auto left=stereo_scene_eye(source,0,6.4,512);
        const auto right=stereo_scene_eye(source,1,6.4,512);
        require(left && right);
        require(std::get<GpuGridDraw>((*left)[0]).eye_x<0);
        require(std::get<GpuGridDraw>((*right)[0]).eye_x>0);
        require(std::get<GpuGridDraw>(source[0]).eye_x==0);
        GpuSceneRecording recording;recording.reset(800,448);
        RasterCommands pending;pending.reset(800,448);
        recording.append_grid(pending,grid);recording.finish(pending);
        Framebuffer frame(800,448);recording.replay(frame,nullptr);
        require(frame.draw_scale()==1);
        require(recording.draws().size()==1);
        const auto points=project_source_grid(grid.camera,grid.matrix,400,224);
        require(points.count>0);
        for(std::size_t i=0;i<points.count;++i) {
            const auto p=points.points[i];
            require(frame.pixels()[unsigned(p.y)*2*800+unsigned(p.x)*2]==126);
        }
        grid.lines=true;grid.line_start={-20,40};
        recording.reset(800,448);pending.reset(800,448);
        recording.append_grid(pending,grid);recording.finish(pending);
        recording.replay(frame,nullptr);
        Framebuffer expected(800,448);expected.set_draw_scale(2);
        DustRenderer::draw_grid_lines_frame({points,grid.line_start},expected);
        require(frame.pixels()==expected.pixels());
        const auto line_eye=stereo_scene_eye(recording.draws(),1,6.4,512);
        require(line_eye && std::get<GpuGridDraw>((*line_eye)[0]).line_start==grid.line_start);
    }
    {
        RasterCommands raster;
        GpuModelDraw model;
        require(!model.identity);
        model.identity=GpuModelIdentity{42,9,123,456,2};
        auto recycled=*model.identity;
        ++recycled.generation;
        require(recycled!=*model.identity);
        auto reshaped=*model.identity;
        ++reshaped.shape;
        require(reshaped!=*model.identity);
        model.pose.x=10;model.pose.z=100;model.pose.animation_frame=7;
        model.pose.source_depth=123;model.pose.use_source_lighting_state=true;
        model.previous_pose=model.pose;model.previous_pose->x=8;
        const std::array<GpuSceneDraw,3> frame{GpuRasterDraw{&raster},model,GpuRasterDraw{&raster}};
        const auto left=stereo_scene_eye(frame,0,6.4,100);
        const auto right=stereo_scene_eye(frame,1,6.4,100);
        require(left && right && left->size()==3 && right->size()==3);
        const auto& l=std::get<GpuModelDraw>((*left)[1]);
        const auto& r=std::get<GpuModelDraw>((*right)[1]);
        require(l.identity==model.identity && r.identity==model.identity);
        require(l.pose.x>model.pose.x && r.pose.x<model.pose.x);
        require(std::abs(l.pose.x*256/100+l.pose.vanish_x-r.pose.x*256/100-r.pose.vanish_x)<1e-12);
        require(l.pose.animation_frame==7 && r.pose.source_depth==123 && r.pose.use_source_lighting_state);
        require(l.pose.continuous_geometry && r.pose.subpixel_projection);
        require(l.previous_pose && r.previous_pose);
        require(std::abs((l.pose.x-l.previous_pose->x)-2)<1e-12);
        require(std::abs((r.pose.x-r.previous_pose->x)-2)<1e-12);
        require(l.previous_pose->vanish_x==l.pose.vanish_x && r.previous_pose->vanish_x==r.pose.vanish_x);
        require(l.previous_pose->continuous_geometry && r.previous_pose->subpixel_projection);
        require(std::get<GpuRasterDraw>((*left)[0]).commands==&raster);
        require(std::get<GpuRasterDraw>((*right)[2]).commands==&raster);
        require(std::get<GpuModelDraw>(frame[1]).pose.x==10);
        require(!stereo_scene_eye(frame,2,6.4,100));
        require(!stereo_scene_eye(frame,0,6.4,0));
    }
    for (const auto size : {std::array<unsigned,2>{1920,1080}, {3840,1080}, {256,224}}) {
        const auto off = stereo_output_layout(StereoOutput::off,size[0],size[1]);
        const auto half = stereo_output_layout(StereoOutput::half_sbs,size[0],size[1]);
        const auto full = stereo_output_layout(StereoOutput::full_sbs,size[0],size[1]);
        require(off && half && full);
        require(off->eye_count == 1 && half->eye_count == 2 && full->eye_count == 2);
        require(half->width == size[0] && full->width == size[0]*2);
        require(half->height == size[1] && full->height == size[1]);
        require(half->scene_aspect == full->scene_aspect && off->scene_aspect == full->scene_aspect);
        for (const auto& layout : {*half,*full}) {
            require(layout.eyes[0].x == 0 && layout.eyes[1].x == layout.eyes[0].width);
            require(layout.eyes[0].width == layout.eyes[1].width);
            require(layout.eyes[1].x + layout.eyes[1].width == layout.width);
        }
    }
    require(!stereo_output_layout(StereoOutput::half_sbs,1919,1080));
    require(!stereo_output_layout(StereoOutput::full_sbs,0xffffffffU,1080));
    require(!stereo_output_layout(StereoOutput::off,1920,0));
    require(!stereo_output_layout(static_cast<StereoOutput>(3),1920,1080));
    const auto eyes = stereo_eye_projections(6.4,100,1.5);
    require(eyes.has_value());
    const auto project = [&](unsigned eye, double x, double z) {
        return 1.5*(x-(*eyes)[eye].eye_x)/z+(*eyes)[eye].projection_offset_x;
    };
    for (double x : {-50.,0.,50.})
        require(std::abs(project(0,x,100)-project(1,x,100)) < 1e-12);
    require(project(0,0,50) > project(1,0,50));
    require(project(0,0,200) < project(1,0,200));
    require(!stereo_eye_projections(0,100,1.5));
    require(!stereo_eye_projections(6.4,-100,1.5));
    require(!stereo_eye_projections(6.4,100,std::numeric_limits<double>::infinity()));
    std::cout << "SBS layout and off-axis stereo projection checks pass\n";
}
