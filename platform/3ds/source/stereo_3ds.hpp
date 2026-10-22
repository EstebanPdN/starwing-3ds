#pragma once

#include "starfox/render/software_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace starfox::platform_3ds {

// Keep the source word raster stable when the analogue slider jitters. The
// two parallel eye cameras converge at 256 source units, without toe-in.
inline std::uint8_t stereo_slider_strength(float slider) noexcept {
    if (!std::isfinite(slider) || slider <= 0.0F) return 0U;
    return static_cast<std::uint8_t>(std::clamp(
        std::lround(std::min(slider, 1.0F) * 8.0F), 1L, 8L));
}

inline void apply_stereo_eye(render::RenderPose& pose, int eye) noexcept {
    pose.x -= eye;
    pose.stereo_eye_offset = eye;
}

// BG2 has no geometry depths. Treat it as the far scenery plane, behind
// every projected world object. Copying it unchanged into both eyes put
// distant mountains on the screen while nearer objects receded behind them.
inline void stereo_background(const render::Framebuffer& source,
    render::Framebuffer& target, int eye) {
    const int width = static_cast<int>(source.width());
    for (unsigned y=0; y<source.height(); ++y) {
        const auto* src=source.pixels().data()+y*source.stored_width();
        auto* dst=target.pixels().data()+y*target.stored_width();
        const int shift=std::clamp(eye,-width,width);
        if (shift>=0) {
            std::fill_n(dst,shift,src[0]);
            std::copy_n(src,width-shift,dst+shift);
        } else {
            std::copy_n(src-shift,width+shift,dst);
            std::fill_n(dst+width+shift,-shift,src[width-1]);
        }
    }
}

// Citro2D has 16-bit vertex indices and four vertices per quad. Reserve one
// foreground image per eye and reject a BG2 plan that cannot fit both views.
inline bool stereo_bg2_quads_fit(std::size_t quads) noexcept {
    return quads <= 8191U;
}

} // namespace starfox::platform_3ds
