#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace starfox::render {
enum class StereoOutput : std::uint8_t { off, half_sbs, full_sbs };
// GPU-only final packing, after independent per-eye effects. Source textures
// are equally sized sampler-capable 2D textures; destination is color-target
// capable, sized by stereo_output_layout(mode,width,height). All handles share
// the caller's SDL GPU device. No active render/copy pass may be open.
// Caller owns command submission and synchronization. No CPU pixel copy.
bool enqueue_stereo_texture_pack(void* command,void* left,void* right,
    void* destination,StereoOutput mode,std::uint32_t width,std::uint32_t height);
struct StereoOutputLayout {
    std::uint32_t width{}, height{}, eye_count{};
    struct Viewport { std::uint32_t x{}, width{}; };
    std::array<Viewport, 2> eyes{};
    // The scene aspect is independent of the squeezed Half-SBS viewport.
    double scene_aspect{};
};
inline std::optional<StereoOutputLayout> stereo_output_layout(
    StereoOutput mode, std::uint32_t width, std::uint32_t height) noexcept {
    if (!width || !height) return {};
    StereoOutputLayout result{width, height, 1, {{{0, width}, {0, 0}}},
        double(width) / height};
    switch (mode) {
    case StereoOutput::off: return result;
    case StereoOutput::half_sbs:
        // Equal-sized eyes are essential to matching disparity. The caller
        // must select an even target size rather than stretch one eye.
        if (width % 2 || width < 2) return {};
        result.eyes = {{{0, width / 2}, {width / 2, width / 2}}};
        break;
    case StereoOutput::full_sbs:
        if (width > std::numeric_limits<std::uint32_t>::max() / 2) return {};
        result.width = width * 2;
        result.eyes = {{{0, width}, {width, width}}};
        break;
    default: return {};
    }
    result.eye_count = 2;
    return result;
}

// Parallel (not toe-in) cameras: translate world X by -eye_x before
// projection, then add projection_offset_x in normalized device space.
// This keeps vertical disparity zero and the convergence plane stationary.
struct StereoEyeProjection { double eye_x{}, projection_offset_x{}; };
inline std::optional<std::array<StereoEyeProjection, 2>> stereo_eye_projections(
    double separation, double convergence, double horizontal_focal_scale) noexcept {
    if (!std::isfinite(separation) || separation <= 0
        || !std::isfinite(convergence) || convergence <= 0
        || !std::isfinite(horizontal_focal_scale) || horizontal_focal_scale <= 0)
        return {};
    const double eye = separation / 2;
    const double offset = horizontal_focal_scale * (eye / convergence);
    if (!std::isfinite(offset) || offset == 0) return {};
    return std::array<StereoEyeProjection, 2>{{{-eye, -offset}, {eye, offset}}};
}
}
