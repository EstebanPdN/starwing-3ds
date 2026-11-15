#pragma once

#include "starfox/assets/shape.hpp"
#include "starfox/assets/rom.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/simulation/math.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <algorithm>
#include <vector>
#include <optional>

// Software renderer for Super FX models: transform, projection, visibility and
// BSP ordering, clipping and scan conversion that follow the cartridge's
// MOBJ.MC routines, into an indexed Framebuffer or its RasterCommands
// recorder. Used by the 3DS frame code, host tools and tests.
namespace starfox::render {

// One model draw: where the model is, how it is oriented and projected, and
// the cartridge material and effect state its routines read. x/y/z is the
// model origin in camera space (source units, +z ahead). The orientation is
// rotation_matrix (Q15) when use_rotation_matrix is set, otherwise
// pitch/yaw/roll in 16-bit binary angles. vanish_x/vanish_y is the projection
// centre in source pixels.
struct RenderPose {
    double x{};
    double y{};
    double z{420.0};
    double pitch{};
    double yaw{};
    double roll{};
    double scale{1.0};
    double vanish_x{112.0};
    double vanish_y{96.0};
    // Parallel eye position; projection clamps disparity into the display.
    std::int32_t stereo_eye_offset{};
    std::uint32_t animation_frame{};
    std::uint32_t colour_frame{};
    std::int32_t texture_scroll_x{};
    std::int32_t texture_scroll_y{};
    // Star Fox EX's M_WIREMODE selects the MDRAWP scanline path: 0 is the
    // retail fill, 1 is hlines23/mhlines3 wireframe, and 2 retains fills until
    // an asymmetric edge transition enters the source's missing-poly path.
    std::uint8_t wireframe_mode{};
    // EX NAN modes 6-9 select the deliberately broken/wavy/cel variants in
    // MDRAWP.MC. The wave phase combines M_SINEOFFSET with M_FRAMENUM.
    std::uint8_t wobble_mode{};
    bool wave_mode{};
    bool cel_mode{};
    std::int16_t wave_offset{};
    // EX's secret COLOR WARP option bypasses the model colour table in
    // MDO_COLOUR_NG and feeds its register PRNG word into the ordinary
    // material decoder.  M_PROJPNTS is the register seed at the start of a
    // normal face pass; retain it as data because EX moves that work buffer.
    bool colour_warp{};
    std::uint16_t projected_points_address{0x0b9fU};
    // afexp/al_count: MOBJ offsets each face along its signed normal as the
    // destroyed model breaks apart.
    std::uint8_t explosion_progress{};
    std::array<std::int16_t, 9> rotation_matrix{};
    bool use_rotation_matrix{};
    // Geometry may be interpolated between source frames for high-FPS
    // presentation, but MOBJ chooses its depth band and light vector only at
    // the completed source draw. Keeping that state separate prevents a
    // presentation frame from dimming a face before the original routine did.
    double source_depth{};
    std::array<std::int16_t, 9> source_lighting_matrix{};
    bool use_source_lighting_state{};
    // Generated presentation frames may retain fractional transformed
    // coordinates. Completed source frames leave this disabled and continue
    // through the bit-exact Super FX word path.
    bool subpixel_projection{};
    // A high-FPS presentation must use the same geometry/visibility path at
    // source boundaries as between them, even when rendering at native 1x.
    // Leave false for exact native-raster audits and 20 FPS presentation.
    bool continuous_geometry{};
    std::array<std::int16_t, 3> depth_thresholds{2'560, 3'328, 3'840};
    std::array<std::array<std::uint8_t, 32>, 4> depth_colour_tables{};
    bool has_depth_colour_tables{};
    // MOBJ.MC replaces every material with colour $09 in CMODE 2 for an
    // ordinary projected shadow. The byte stores its alternating 4-bpp colours.
    bool force_colour{};
    std::uint8_t forced_colour{};
    // Host HUD geometry may use a dedicated full CGRAM index, unlike native
    // shadow materials which use two packed four-bit colors above.
    std::optional<std::uint8_t> palette_override;
    bool simple_scaled_sprite{};
    std::uint8_t simple_sprite_colour{};
    std::int16_t simple_sprite_world_size{};
    // Optional presentation clip for transient sprite/particle effects. A
    // non-empty interval keeps those effects inside the source camera while
    // allowing the surrounding 3D scene to use a wider host framebuffer.
    std::int32_t effect_clip_left{};
    std::int32_t effect_clip_right{};
    // Presentation-only fallback for a long tapered solid crossing the near
    // plane. It retains the model's material while drawing its centre axis.
    bool collapse_to_axis_line{};
};

// Loads MOBJ's depth cue into `pose`: three depth thresholds and four 32-byte
// depth colour tables from the ROM bank of depth_table_address (DEPTHTABLES).
// A non-zero object_depth_offset reads the thresholds from DEPTHTABLES +
// (offset - 1) * 4 instead of threshold_pointer. A RAM bank or a pointer
// below $8000 only clears has_depth_colour_tables.
void apply_source_depth_tables(
    const assets::RomImage& rom,
    std::uint32_t depth_table_address,
    std::uint16_t threshold_pointer,
    std::uint16_t colour_pointer,
    std::uint8_t object_depth_offset,
    RenderPose& pose);

struct RenderSettings {
    // MOBJ.MC projects with (coordinate * 256) / z before adding the
    // 112x96 vanishing point.
    double focal_length{256.0};
    // Retail shapes carry explicit Super FX visibility triples (and BSP
    // ordering). Applying a second vertex-winding test hides their selected
    // filled faces, so source rendering leaves generic culling disabled.
    bool backface_culling{false};
    // draw() clears to background_colour; colour_index_base is added to every
    // material colour and texel index.
    std::uint8_t background_colour{};
    std::uint8_t colour_index_base{};
    // Supersampling factor for polygon projection and scan conversion. The
    // fractional geometry shared by rasterization, visibility and BSP order
    // stays scale-independent and stable across source-frame boundaries.
    std::uint32_t render_scale{1U};
    // Width (1-4) of two-vertex faces and the axis-line fallback, widened
    // with render_scale up to 4x.
    std::uint8_t wireframe_thickness{1U};
};

// Presentation metadata for a host-rendered Super FX surface. The indexed
// framebuffer remains source-accurate; this parallel buffer gives optional PC
// rendering effects the actual polygon normal and depth instead of asking a
// screen-space filter to guess them from palette brightness.
struct SurfaceSample {
    float normal_x{};
    float normal_y{};
    float normal_z{1.0F};
    float depth{};
    std::uint8_t palette_index{};
    bool valid{};
};

// SurfaceSample plane at a framebuffer's stored resolution. set() records the
// written bounding box so clear() resets only that region; set() ignores
// out-of-range pixels and get() is unchecked.
class SurfaceBuffer {
public:
    SurfaceBuffer(std::uint32_t width, std::uint32_t height)
        : width_(width), height_(height), samples_(
              static_cast<std::size_t>(width) * height) {}

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::span<const SurfaceSample> samples() const noexcept { return samples_; }

    void resize(std::uint32_t width, std::uint32_t height) {
        if (width == width_ && height == height_) return;
        width_ = width;
        height_ = height;
        samples_.assign(static_cast<std::size_t>(width) * height, {});
        reset_bounds();
    }

    void clear() noexcept {
        if (!empty()) {
            for (auto y = minimum_y_; y < maximum_y_; ++y) {
                const auto first = samples_.begin()
                    + static_cast<std::ptrdiff_t>(
                        static_cast<std::size_t>(y) * width_ + minimum_x_);
                std::fill(first, first + static_cast<std::ptrdiff_t>(
                    maximum_x_ - minimum_x_), SurfaceSample{});
            }
        }
        reset_bounds();
    }

    void set(std::int32_t x, std::int32_t y, SurfaceSample sample,
        std::uint8_t palette_index) noexcept {
        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(width_)
            || y >= static_cast<std::int32_t>(height_)) {
            return;
        }
        sample.palette_index = palette_index;
        sample.valid = true;
        samples_[static_cast<std::size_t>(y) * width_
            + static_cast<std::size_t>(x)] = sample;
        minimum_x_ = std::min(minimum_x_, static_cast<std::uint32_t>(x));
        minimum_y_ = std::min(minimum_y_, static_cast<std::uint32_t>(y));
        maximum_x_ = std::max(maximum_x_, static_cast<std::uint32_t>(x) + 1U);
        maximum_y_ = std::max(maximum_y_, static_cast<std::uint32_t>(y) + 1U);
    }

    [[nodiscard]] const SurfaceSample& get(
        std::uint32_t x, std::uint32_t y) const noexcept {
        return samples_[static_cast<std::size_t>(y) * width_ + x];
    }

    [[nodiscard]] bool empty() const noexcept {
        return minimum_x_ >= maximum_x_ || minimum_y_ >= maximum_y_;
    }
    [[nodiscard]] std::uint32_t maximum_y() const noexcept { return maximum_y_; }

private:
    void reset_bounds() noexcept {
        minimum_x_ = width_;
        minimum_y_ = height_;
        maximum_x_ = 0U;
        maximum_y_ = 0U;
    }

    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<SurfaceSample> samples_;
    std::uint32_t minimum_x_{width_};
    std::uint32_t minimum_y_{height_};
    std::uint32_t maximum_x_{};
    std::uint32_t maximum_y_{};
};

// Optional diagnostic capture; never changes rasterization or production output.
struct PolygonRenderDiagnostics {
    std::vector<std::array<double,3>> camera;
    std::vector<std::array<double,2>> projected;
    double signed_area{};
};
// camera/projected/clipped/visible trace the collapse_to_axis_line axis (near
// endpoint first); polygons records every projected polygon face.
struct RenderDiagnostics {
    std::array<std::array<double,3>,2> camera{};
    std::array<std::array<double,2>,2> projected{},clipped{};
    bool visible{};
    std::vector<PolygonRenderDiagnostics> polygons;
};

// Optional caller-owned buffers for serialized model draws. A platform frame
// renderer can retain their capacity across many shapes instead of allocating
// the same projection/clipping vectors for every model.
namespace software_renderer_detail {
struct Vec3 { double x{}, y{}, z{}; };
struct ScreenPoint { double x{}, y{}, z{}; bool visible{}; };
struct TexturePoint { double u{}, v{}; };
struct RasterVertex { ScreenPoint point{}; TexturePoint texture{}; };
struct WordProjection { std::int16_t x{}, y{}, z{}; };
struct RasterPoint { std::int32_t x{}, y{}; };
struct Scratch {
    std::vector<Vec3> transformed_vertices;
    std::vector<ScreenPoint> projected;
    std::vector<const assets::Face*> ordered_faces;
    std::vector<Vec3> camera_polygon;
    std::vector<Vec3> camera_clipping_scratch;
    std::vector<ScreenPoint> polygon;
    std::vector<RasterVertex> raster_polygon;
    std::vector<RasterVertex> clipping_scratch;
    // Exact cartridge geometry can stay integral through projection,
    // visibility and solid-face clipping. Retain these allocations too.
    std::vector<WordProjection> word_projected;
    std::vector<RasterPoint> word_polygon;
    std::vector<RasterPoint> word_clipping_scratch;
};
} // namespace software_renderer_detail

class SoftwareRenderer {
public:
    explicit SoftwareRenderer(RenderSettings settings = {});

    // Draws one shape at `pose` into `target`, whose draw scale is expected
    // to equal settings.render_scale. clear_target first clears the target to
    // the background colour, and the surfaces. surfaces, when given, receives
    // the normal and depth of every polygon pixel; axis_diagnostics records
    // the projection trace. reusable_scratch keeps the per-draw vectors'
    // capacity across calls and must not be shared by concurrent draws.
    void draw(
        const assets::Shape& shape,
        const RenderPose& pose,
        Framebuffer& target,
        bool clear_target = true,
        SurfaceBuffer* surfaces = nullptr,
        RenderDiagnostics* axis_diagnostics = nullptr,
        software_renderer_detail::Scratch* reusable_scratch = nullptr) const;

    // MHUD.MC's first-person direction indicators are a Super FX line pass,
    // separate from both the 3D object list and the SNES OAM reticle.
    // rotation is an 8-bit angle (256 per turn; fractions interpolate the
    // trig tables). damage_flags bits 1 and 0 draw the positive and negative
    // halves in the damage colour. horizontal_origin is the left edge of the
    // 224-pixel window; a non-zero normal_colour_override replaces the normal
    // colour.
    void draw_cockpit_hud(
        const simulation::TrigTables& trigonometry,
        double rotation,
        std::uint8_t colour,
        std::uint8_t damage_flags,
        std::int32_t horizontal_origin,
        Framebuffer& target,
        std::uint8_t normal_colour_override = 0U) const;

private:
    void draw_impl(const assets::Shape&,const RenderPose&,Framebuffer&,bool,
        SurfaceBuffer*,RenderDiagnostics*,
        software_renderer_detail::Scratch*) const;
    RenderSettings settings_;
};

} // namespace starfox::render
