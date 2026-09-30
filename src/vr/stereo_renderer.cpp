#include "starfox/vr/stereo_renderer.hpp"
namespace starfox::vr {
StereoRenderer::Result StereoRenderer::step(const DrawEye& draw,
    float units, float near_plane, std::optional<float> far_plane) {
    return step_async([&](unsigned eye,uint32_t image,const EyeCamera& camera,XrTime time) {
        try {return draw && draw(eye,image,camera,time)?EyeResult::complete:EyeResult::failed;}
        catch(...) {return EyeResult::failed;}
    },units,near_plane,far_plane);
}
StereoRenderer::Result StereoRenderer::step_async(const AsyncDrawEye& draw,
    float units,float near_plane,std::optional<float> far_plane) {
    if(fatal_) return Result::error;
    if(!frame_) {
        if(!session_.poll_events()) return Result::error;
        if(!session_.running() || session_.exit_requested()) return Result::idle;
        frame_=session_.begin_frame();
        if(!frame_) return Result::error;
        next_eye_=0;cancelling_=false;
        if(frame_->tracking_origin_changed) position_anchor_.reset();
        if(!frame_->should_render) {
            frame_.reset();
            return session_.end_frame()?Result::skipped:Result::error;
        }
        auto scene_views=frame_->views;
        if(anchor_position_ && !position_anchor_.apply(scene_views)) cancelling_=true;
        for(unsigned eye=0;eye<2 && !cancelling_;++eye) {
            const auto camera=eye_camera(scene_views[eye],units,near_plane,far_plane);
            if(!camera) {cancelling_=true;break;}
            cameras_[eye]=*camera;
        }
        if(!images_.start_frame(*frame_,session_.space())) cancelling_=true;
    }
    while(!cancelling_ && next_eye_<2) {
        const auto ready=images_.acquire_eye(next_eye_);
        if(ready==ImageWait::waiting) return Result::waiting;
        if(ready==ImageWait::error) {cancelling_=true;break;}
        auto rendered=EyeResult::failed;
        try {
            if(draw) rendered=draw(next_eye_,*images_.image_index(next_eye_),
                cameras_[next_eye_],frame_->display_time);
        } catch(...) {rendered=EyeResult::fatal;}
        if(rendered==EyeResult::fatal) {fatal_=true;return Result::error;}
        if(rendered==EyeResult::pending) return Result::waiting;
        if(rendered==EyeResult::failed || !images_.release_eye(next_eye_)) {cancelling_=true;break;}
        ++next_eye_;
    }
    if(cancelling_) {
        const auto cancelled=images_.cancel_frame();
        if(cancelled==ImageWait::waiting) return Result::waiting;
        if(cancelled==ImageWait::error) return Result::error;
        frame_.reset();session_.end_frame();return Result::error;
    }
    const auto* projection=images_.projection();
    if(!projection) {
        frame_.reset();session_.end_frame();return Result::error;
    }
    const XrCompositionLayerBaseHeader* layers[]{
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(projection)};
    frame_.reset();
    return session_.end_frame(layers)?Result::submitted:Result::error;
}
}
