#include "starfox/vr/eye_camera.hpp"
#include "starfox/render/stereo_output.hpp"
#include <cmath>
#include <numbers>
namespace starfox::vr {
std::optional<std::array<EyeCamera,2>> sbs_eye_cameras(float vertical_fov,
    float aspect,float separation,float convergence,float near_plane,
    std::optional<float> far_plane) noexcept {
    if(!std::isfinite(vertical_fov) || vertical_fov<=0
        || vertical_fov>=std::numbers::pi_v<float>
        || !std::isfinite(aspect) || aspect<=0) return {};
    const double top=std::tan(double(vertical_fov)*.5);
    const double right=top*aspect;
    XrView centre{XR_TYPE_VIEW};centre.pose.orientation.w=1;
    centre.fov={float(-std::atan(right)),float(std::atan(right)),
        vertical_fov*.5F,-vertical_fov*.5F};
    const auto base=eye_camera(centre,1,near_plane,far_plane);
    if(!base) return {};
    const auto offsets=render::stereo_eye_projections(separation,convergence,base->projection[0]);
    if(!offsets) return {};
    std::array<EyeCamera,2> result{*base,*base};
    for(unsigned eye=0;eye<2;++eye) {
        result[eye].view[12]=float(-(*offsets)[eye].eye_x);
        // Forward is -Z and clip W is -Z, hence the negative matrix term.
        result[eye].projection[8]=float(-(*offsets)[eye].projection_offset_x);
        if(!std::isfinite(result[eye].projection[8])) return {};
    }
    return result;
}
bool PositionAnchor::apply(std::array<XrView,2>& views) noexcept {
    for(const auto& eye:views) for(float value:{eye.pose.position.x,eye.pose.position.y,eye.pose.position.z})
        if(!std::isfinite(value)) return false;
    if(!origin_) origin_=XrVector3f{
        views[0].pose.position.x*.5F+views[1].pose.position.x*.5F,
        views[0].pose.position.y*.5F+views[1].pose.position.y*.5F,
        views[0].pose.position.z*.5F+views[1].pose.position.z*.5F};
    for(auto& eye:views) {
        eye.pose.position.x-=origin_->x;
        eye.pose.position.y-=origin_->y;
        eye.pose.position.z-=origin_->z;
    }
    return true;
}
std::optional<EyeCamera> model_eye_camera(const EyeCamera& camera,const Matrix4& model) noexcept {
    for(const auto* matrix:{&camera.view,&camera.projection,&model})
        for(float value:*matrix) if(!std::isfinite(value)) return {};
    if(model[3]!=0 || model[7]!=0 || model[11]!=0 || model[15]!=1) return {};
    EyeCamera result{};result.projection=camera.projection;result.effects=camera.effects;
    for(unsigned column=0;column<4;++column) for(unsigned row=0;row<4;++row) {
        double value=0;
        for(unsigned k=0;k<4;++k) value+=double(camera.view[k*4+row])*model[column*4+k];
        result.view[column*4+row]=static_cast<float>(value);
        if(!std::isfinite(result.view[column*4+row])) return {};
    }
    return result;
}
std::optional<EyeCamera> eye_camera(const XrView& eye,float units,float near,
    std::optional<float> far) noexcept {
    if(!std::isfinite(units) || units<=0 || !std::isfinite(near) || near<=0
        || (far && (!std::isfinite(*far) || *far<=near))) return {};
    const auto& p=eye.pose.position;
    const auto& q=eye.pose.orientation;
    for(float value:{p.x,p.y,p.z,q.x,q.y,q.z,q.w}) if(!std::isfinite(value)) return {};
    const auto& f=eye.fov;
    for(float angle:{f.angleLeft,f.angleRight,f.angleDown,f.angleUp})
        if(!std::isfinite(angle) || std::abs(angle)>=std::numbers::pi_v<float>/2) return {};
    if(f.angleLeft>=f.angleRight || f.angleDown>=f.angleUp) return {};
    const double norm=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
    if(norm<1e-12) return {};
    // Conjugate the normalized eye-to-world quaternion to obtain world-to-eye.
    const double inverse=1/std::sqrt(norm);
    const double x=-q.x*inverse,y=-q.y*inverse,z=-q.z*inverse,w=q.w*inverse;
    EyeCamera result{};
    auto& v=result.view;
    v[0]=float(1-2*(y*y+z*z));v[4]=float(2*(x*y-z*w));v[8]=float(2*(x*z+y*w));
    v[1]=float(2*(x*y+z*w));v[5]=float(1-2*(x*x+z*z));v[9]=float(2*(y*z-x*w));
    v[2]=float(2*(x*z-y*w));v[6]=float(2*(y*z+x*w));v[10]=float(1-2*(x*x+y*y));
    for(unsigned row=0;row<3;++row)
        v[12+row]=float(-(double(v[row])*p.x+double(v[4+row])*p.y+double(v[8+row])*p.z)*units);
    v[15]=1;
    const double left=std::tan(double(f.angleLeft)),right=std::tan(double(f.angleRight));
    const double down=std::tan(double(f.angleDown)),up=std::tan(double(f.angleUp));
    auto& m=result.projection;
    m[0]=float(2/(right-left));m[5]=float(-2/(up-down));
    m[8]=float((right+left)/(right-left));m[9]=float(-(up+down)/(up-down));
    m[10]=far?float(double(*far)/(double(near)-*far)):-1;
    m[14]=far?float(double(*far)*near/(double(near)-*far)):-near;
    m[11]=-1;
    for(const auto& matrix:{v,m}) for(float value:matrix) if(!std::isfinite(value)) return {};
    return result;
}
}
