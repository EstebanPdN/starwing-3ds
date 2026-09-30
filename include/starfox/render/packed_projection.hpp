#pragma once
#include "starfox/render/gpu_projection.hpp"
#include "starfox/render/software_renderer.hpp"
namespace starfox::render {
struct PackedProjection {
    bool continuous{};
    std::vector<NativeTransformVertex> native_vertices;
    NativeTransformPose native_pose{};
    std::vector<ContinuousTransformVertex> continuous_vertices;
    // Byte vertices use pose 0; word vertices use pose 1, bypassing both
    // object scale and header shift exactly as the source renderer does.
    // Poses 2/3 carry coefficient residuals for poses 0/1.
    std::array<ContinuousTransformPose,4> continuous_poses{};
    // High/low source Euler operands for a sequential X/Y/Z GPU transform.
    // translation.xyz hold third residuals: cosine in operand 0, sine in 1.
    // Separate from matrix poses so existing destruction records keep layout.
    std::array<ContinuousTransformPose,2> euler_operands{};
    std::vector<std::array<std::uint32_t,4>> visibility_faces;
};
// Package the selected animation frame for GPU transform/projection. No CPU
// rotation or projection is performed per vertex. Native pre-scale/word
// quantization is currently done here; continuous scale stays in GPU matrices.
// Currently accepts source Q15 poses and continuous Q15/Euler poses. Rejects
// other projection/visibility combinations instead of silently changing them.
[[nodiscard]] PackedProjection pack_projection(const assets::Shape&,const RenderPose&,const RenderSettings&);
// Rigid temporal correspondence using the same packed transforms as the GPU.
// Rejects changed topology/coordinates, native word wrapping, singular matrices,
// and mixed byte/word transforms that cannot share one camera-space mapping.
[[nodiscard]] std::optional<GpuProjection::MotionSurfaceSettings> pack_motion_surface(
    const PackedProjection& current,const PackedProjection& previous,
    std::uint32_t width,std::uint32_t height,std::uint32_t scale);
// Axis-collapse reduction groups in source order: maximum authored Z first,
// minimum authored Z second. GPU reduction must average transformed positions,
// not an object-space centroid (word wrapping can make those differ).
[[nodiscard]] std::array<std::vector<std::uint32_t>,2> pack_axis_groups(
    const assets::Shape&,std::uint32_t animation_frame);
}
