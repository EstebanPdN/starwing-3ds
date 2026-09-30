#pragma once
#include "starfox/vr/eye_camera.hpp"
#include "starfox/vr/openxr_swapchains.hpp"
#include <functional>

namespace starfox::vr {
// Drives one predicted stereo frame using an API-specific eye renderer.
// The callback must finish its GPU work before returning (including false).
// No simulation tick belongs in this callback: both eyes see the same scene.
class StereoRenderer {
public:
    using DrawEye = std::function<bool(unsigned, uint32_t, const EyeCamera&, XrTime)>;
    enum class EyeResult {complete, pending, failed, fatal};
    // Pending callbacks are retried with the same image/camera/time. Failed
    // means no GPU work remains; fatal retains images until device teardown.
    using AsyncDrawEye = std::function<EyeResult(unsigned,uint32_t,const EyeCamera&,XrTime)>;
    StereoRenderer(OpenXrSession& session, OpenXrSwapchains& images,bool anchor_position=false)
        :session_(session),images_(images),anchor_position_(anchor_position) {}
    enum class Result {idle, waiting, submitted, skipped, error};
    Result step(const DrawEye&, float units_per_metre, float near_plane,
        std::optional<float> far_plane=std::nullopt);
    Result step_async(const AsyncDrawEye&,float units_per_metre,float near_plane,
        std::optional<float> far_plane=std::nullopt);
    bool teardown_required() const noexcept {return fatal_;}
    XrPosef anchored_pose(XrPosef pose) const noexcept {return anchor_position_?position_anchor_.anchored(pose):pose;}
private:
    OpenXrSession& session_;
    OpenXrSwapchains& images_;
    std::optional<StereoFrame> frame_;
    std::array<EyeCamera,2> cameras_{};
    unsigned next_eye_{};
    bool cancelling_{};
    bool fatal_{};
    bool anchor_position_{};
    PositionAnchor position_anchor_;
};
}
