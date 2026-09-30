#pragma once
#include "starfox/render/gpu_raster.hpp"
#include "starfox/render/gpu_background.hpp"
#include "starfox/render/grid_projection.hpp"
#include "starfox/render/dust_renderer.hpp"
#include "starfox/render/particle_renderer.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include <span>
#include <variant>
#include <deque>
namespace starfox::render {
// Source slot alone is not an entity identity: the native pool recycles it.
// Shape/strategy/type changes also break temporal correspondence. Renderers
// must additionally reset history on scene/load-state changes and missed frames.
struct GpuModelIdentity {
    std::uint16_t slot{};
    std::uint64_t generation{};
    std::uint16_t shape{};
    std::uint32_t strategy{};
    std::uint8_t type{};
    bool operator==(const GpuModelIdentity&) const = default;
};
struct GpuModelDraw {
    const assets::Shape* shape{};
    RenderPose pose{};
    RenderSettings settings{};
    bool surface_metadata{};
    // Unidentified draws (shadows, title helpers, etc.) must not silently share
    // temporal history by shape pointer or draw order. The caller validates
    // identity/history before supplying previous_pose for motion generation.
    std::optional<GpuModelIdentity> identity;
    bool geometry_depth{};
    bool ray_geometry{}; // Opt-in caster, never inferred from HUD/native shadows.
    std::optional<RenderPose> previous_pose; // Validated presentation history only.
    // Current raster-pixel displacement. History poses remain unjittered.
    std::array<float,2> raster_jitter{};
};
struct GpuRasterDraw {
    RasterCommands* commands{};
    bool surface_metadata{};
    bool gpu_binning{};
};
struct GpuGridDraw {
    timing::RenderTransform camera{};
    simulation::MatrixQ15 matrix{};
    std::uint32_t scale{1};
    std::uint8_t colour{126};
    float eye_x{},convergence{512};
    bool lines{};
    std::array<std::int16_t,2> line_start{};
};
struct GpuDustDraw {
    DustRenderer::DustFrame frame;
    std::uint32_t scale{1};
    float eye_x{},convergence{512};
};
struct GpuParticleDraw {
    ParticleRenderer::OwnerFrame frame;
    std::uint32_t scale{1};
    float eye_x{},convergence{512};
};
struct GpuTextDraw {
    ScaledTextRenderer::ProjectedFrame frame;
    std::uint32_t scale{1};
    float eye_x{},convergence{512};
};
struct GpuBackgroundDraw {
    std::shared_ptr<const simulation::SnesPpuState> ppu;
    GpuBackgroundSettings settings;
    std::uint32_t scale{1};
};
using GpuSceneDraw = std::variant<GpuModelDraw, GpuRasterDraw, GpuGridDraw, GpuDustDraw, GpuParticleDraw, GpuTextDraw, GpuBackgroundDraw>;
// Copies one ordered frame into an eye view. Raster chunks remain shared at
// the convergence plane; model transforms are eye-specific. Source references
// must outlive submission. Source lighting/animation state is never advanced.
std::optional<std::vector<GpuSceneDraw>> stereo_scene_eye(
    std::span<const GpuSceneDraw>,unsigned eye,double separation,double convergence);
// Records the boundaries between native raster work and deferred GPU models.
// Shape references must survive submission or replay. Raster chunks own their
// texels; moving the pending chunk keeps the framebuffer's recorder address
// stable. Finish recording before consuming draws().
class GpuSceneRecording {
public:
    GpuSceneRecording()=default;
    GpuSceneRecording(const GpuSceneRecording&)=delete;
    GpuSceneRecording& operator=(const GpuSceneRecording&)=delete;
    void reset(std::uint32_t width,std::uint32_t height);
    void append_model(RasterCommands& pending,GpuModelDraw draw);
    void append_grid(RasterCommands& pending,GpuGridDraw draw);
    void append_dust(RasterCommands& pending,GpuDustDraw draw);
    void append_particles(RasterCommands& pending,GpuParticleDraw draw);
    void append_text(RasterCommands& pending,GpuTextDraw draw);
    void append_background(RasterCommands& pending,GpuBackgroundDraw draw);
    void finish(RasterCommands& pending);
    [[nodiscard]] std::span<const GpuSceneDraw> draws() const noexcept {return draws_;}
    // Full ordered fallback, including models not supported by GPU packing.
    void replay(Framebuffer&,SurfaceBuffer*) const;
private:
    void flush(RasterCommands& pending);
    std::uint32_t width_{},height_{};
    std::deque<RasterCommands> raster_;
    std::vector<GpuSceneDraw> draws_;
};
// Resident painter merge. Inputs must carry explicit pixel coverage in bit 26,
// as produced by GpuModel or enqueue_row_spans(pixel_coverage=true).
class GpuScene {
public:
    struct RayGeometryOutput {
        void* device{};void* buffer{};
        std::uint32_t vertex_count{};
        bool complete{}; // False means use the full fallback, not partial casters.
    };
    GpuScene();~GpuScene();
    // Same-size/device inputs. Null background starts a cleared scene. Ping-pong
    // storage allows output from the preceding call as the next background.
    // No upload/submit/readback; caller owns command, cancels it on failure.
    // Consume borrowed outputs before their ping-pong slot is reused.
    GpuRasterOutput enqueue(void* command,const GpuRasterOutput& front,const GpuRasterOutput* back=nullptr);
    // Ordered mixed model/legacy batch. Width/height are final pixel dimensions;
    // model logical dimensions are divided by its render_scale. Referenced
    // shapes/commands need only survive this call. No CPU geometry conversion,
    // submit or readback. Empty input produces a cleared scene. Cancel the
    // caller-owned command on failure (including partially encoded batches).
    // Consume the result before another operation on this scene instance.
    GpuRasterOutput enqueue_batch(void* device,void* command,std::uint32_t width,
        std::uint32_t height,std::span<const GpuSceneDraw> draws);
    // Owned submission for presentation. No readback; retains at most two older
    // submissions, cycling buffer storage on reuse. Borrowed enqueue calls reject
    // pending owned work until wait_for_completion succeeds.
    bool render_resident(void* device,std::uint32_t width,std::uint32_t height,
        std::span<const GpuSceneDraw> draws);
    [[nodiscard]] GpuRasterOutput resident_output() const noexcept;
    // Triangle float4s, concatenated in draw order, borrowed through the next
    // batch/release. Submission ownership/fence rules match the colour output.
    // No requested casters or unsupported caster paths return an empty output.
    [[nodiscard]] RayGeometryOutput ray_geometry_output() const noexcept;
    bool wait_for_completion();
    // Compatibility composition only. Downloads the completed scene without
    // running model transforms/rasterization again on the CPU.
    bool readback(Framebuffer&,SurfaceBuffer*);
    void release_device()noexcept;
    const std::string& status()const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
// Separate scene instances retain both borrowed outputs simultaneously.
// Both eyes enqueue into one caller-owned command: submit once, or cancel the
// entire command if either eye fails. No simulation tick or readback here.
class GpuStereoScene {
public:
    bool render_resident(void* device,std::uint32_t eye_width,std::uint32_t eye_height,
        std::span<const GpuSceneDraw> frame,double separation,double convergence);
    [[nodiscard]] GpuRasterOutput resident_output(unsigned eye) const noexcept {
        return resident_ready_ && eye<2 ? eyes_[eye].resident_output() : GpuRasterOutput{};
    }
    [[nodiscard]] GpuScene::RayGeometryOutput ray_geometry_output(unsigned eye) const noexcept {
        return resident_ready_ && eye<2 ? eyes_[eye].ray_geometry_output() : GpuScene::RayGeometryOutput{};
    }
    std::optional<std::array<GpuRasterOutput,2>> enqueue(void* device,void* command,
        std::uint32_t eye_width,std::uint32_t eye_height,
        std::span<const GpuSceneDraw> frame,double separation,double convergence);
    void release_device() noexcept { resident_ready_=false;for(auto& eye:eyes_) eye.release_device(); }
private:
    std::array<GpuScene,2> eyes_;
    bool resident_ready_{};
};
}
