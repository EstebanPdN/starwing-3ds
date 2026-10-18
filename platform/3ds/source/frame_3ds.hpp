#pragma once

#include "starfox/assets/shape_decoder.hpp"
#include "starfox/render/background_renderer.hpp"
#include "starfox/render/dust_renderer.hpp"
#include "starfox/render/object_snapshot.hpp"
#include "starfox/render/particle_renderer.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include "starfox/render/software_renderer.hpp"
#include "starfox/render/sprite_renderer.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "native_canvas.hpp"
#include "sprite_source_cache.hpp"
#include "bg2_motion.hpp"
#include "native_engine/scene.hpp"

#include <cstdint>
#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace starfox::platform_3ds {

// Cartridge foreground retains its 256x224 raster, centred at (72, 8).
// Gameplay geometry and scenery share the complete 400x240 top screen.
class Frame3ds {
public:
    struct DrawBreakdown {
        float background_ms{};
        float world_ms{};
        float finish_ms{};
        std::uint32_t world_prepare_us{};
        std::uint32_t world_raster_us{};
        std::uint32_t world_composite_us{};
        std::uint32_t native_model_us{};
        std::uint32_t finalize_us{};
    };

    Frame3ds(const assets::RomImage& rom, const assets::SymbolMap& symbols);
    void set_native_engine(bool enabled, bool mode1=false) noexcept {
        native_engine_enabled_=enabled; native_mode1_enabled_=mode1;
    }
    const render::Framebuffer& native_background() const noexcept { return native_background_; }
    const native_engine::Scene* native_scene() const noexcept { return native_scene_active_ ? &native_scene_ : nullptr; }
    void set_stereo_strength(std::uint8_t strength) noexcept;
    [[nodiscard]] bool stereo_active() const noexcept { return last_draw_stereo_; }
    [[nodiscard]] std::uint8_t stereo_strength() const noexcept { return stereo_strength_; }
    [[nodiscard]] const render::Framebuffer& right_frame() const noexcept {
        return last_draw_stereo_ ? right_frame_ : last_frame();
    }
    void set_top_hud_visible(bool visible) noexcept {
        if(show_top_hud_!=visible) {
            show_top_hud_=visible;
            last_draw_valid_=false;
        }
    }
    [[nodiscard]] const render::Framebuffer& draw(
        const simulation::GameSimulation& game, bool gpu_bg2 = false,
        double interpolation_alpha = 1.0,
        const simulation::SnesPpuState* prepared_background = nullptr);
    [[nodiscard]] bool can_reuse_presentation(
        const simulation::GameSimulation& game, bool gpu_bg2,
        double interpolation_alpha = 1.0) const;
    void capture_after_tick(const simulation::GameSimulation& game);
    [[nodiscard]] const render::Framebuffer& last_frame() const noexcept {
        return last_frame_wide_ ? wide_frame_ : frame_;
    }
    // Decode the stage's currently active geometry while the source display
    // is black, before the first visible gameplay presentation.
    [[nodiscard]] unsigned preload_active_shapes(
        const simulation::GameSimulation& game);
    struct PreloadStats { unsigned candidates{}, decoded{}, rejected{}; std::size_t bytes{}; bool limited{}; };
    using PreloadProgress = bool (*)(void*, unsigned, unsigned);
    [[nodiscard]] PreloadStats preload_catalog(std::size_t budget,
        PreloadProgress progress = nullptr, void* context = nullptr);
    [[nodiscard]] std::int16_t background_scroll_x() const noexcept {
        return background_scroll_x_;
    }
    [[nodiscard]] std::int16_t background_scroll_y() const noexcept {
        return background_scroll_y_;
    }
    [[nodiscard]] std::pair<std::int16_t, std::int16_t>
    current_background_scroll(const simulation::GameSimulation& game,
        double interpolation_alpha = 1.0) const;
    // Prepare once before the BG2 worker reads this immutable snapshot. Pass
    // it to draw() so that CPU rendering cannot rewrite the worker's input.
    [[nodiscard]] const simulation::SnesPpuState& presentation_ppu(
        const simulation::GameSimulation& game, double interpolation_alpha);
    [[nodiscard]] std::uint64_t world_cache_hits() const noexcept {
        return world_cache_hits_;
    }
    [[nodiscard]] std::uint64_t world_cache_misses() const noexcept {
        return world_cache_misses_;
    }
    [[nodiscard]] std::uint64_t top_hud_cache_hits() const noexcept {
        return top_hud_cache_hits_;
    }
    [[nodiscard]] std::uint64_t top_hud_cache_misses() const noexcept {
        return top_hud_cache_misses_;
    }
    [[nodiscard]] std::uint64_t background_cache_hits() const noexcept {
        return background_cache_hits_;
    }
    [[nodiscard]] std::uint64_t background_cache_misses() const noexcept {
        return background_cache_misses_;
    }
    [[nodiscard]] DrawBreakdown last_draw_breakdown() const noexcept {
        return last_draw_breakdown_;
    }
    [[nodiscard]] std::uint64_t world_cache_validation_us() const noexcept {
        return world_cache_validation_us_;
    }
    [[nodiscard]] std::uint64_t world_cache_snapshot_us() const noexcept {
        return world_cache_snapshot_us_;
    }
    struct ShapeCacheStats {
        std::size_t bytes{}, budget{}, entries{}, rejected_entries{};
        std::uint64_t evictions{};
    };
    [[nodiscard]] ShapeCacheStats shape_cache_stats() const noexcept {
        return {shape_cache_bytes_, shape_cache_limit_, shapes_.size()+lod_shapes_.size(),
            invalid_shapes_.size()+invalid_lods_.size(), shape_cache_evictions_};
    }

    struct SpriteCacheStats {
        std::uint64_t foreground_hits{}, foreground_misses{};
        std::uint64_t foreground_selective_hits{}, hud_selective_hits{};
        std::size_t bytes{}, foreground_characters{}, hud_characters{};
    };
    [[nodiscard]] SpriteCacheStats sprite_cache_stats() const noexcept {
        return {source_foreground_cache_hits_, source_foreground_cache_misses_,
            source_foreground_selective_hits_, top_hud_selective_hits_,
            sizeof(source_sprite_cache_) + sizeof(hud_sprite_cache_),
            source_sprite_cache_.character_count(), hud_sprite_cache_.character_count()};
    }

    struct StorageStats {
        std::size_t framebuffer_capacity_bytes{}, item_capacity_bytes{};
        std::size_t draw_order_capacity_bytes{}, object_capacity_bytes{};
        std::size_t foreground_spans_size{}, foreground_spans_capacity{};
        std::size_t hud_spans_size{}, hud_spans_capacity{};
        std::size_t spans_capacity_bytes{}, scratch_capacity_bytes{};
        std::size_t interpolation_entries{}, interpolation_buckets{};
        std::size_t fixed_particle_dust_bytes{}, tracked_vector_capacity_bytes{};
        std::size_t bg2_presentation_fixed_bytes{};
    };
    // On-demand dump inventory: exact vector payload capacities, not an
    // allocator/heap total. Map node allocations and shape data are separate.
    [[nodiscard]] StorageStats storage_stats() const noexcept;

private:
    bool native_engine_enabled_{}, native_mode1_enabled_{}, native_scene_active_{}, last_native_world_{};
    render::RasterCommands native_commands_;
    render::Framebuffer native_background_{0,0}, empty_native_{0,0};
    native_engine::Scene native_scene_;
    struct Mode1BackgroundCache {
        render::Framebuffer bg3_low{256U, 224U};
        render::Framebuffer bg3_high{256U, 224U};
        render::Framebuffer panorama_low{400U, 240U};
        render::Framebuffer panorama_high{400U, 240U};
        render::Framebuffer bg2_low{256U, 224U};
        render::Framebuffer bg2_high{256U, 224U};
        render::Framebuffer bg1{256U, 224U};
        simulation::SnesPpuState ppu{};
        std::array<std::uint16_t, 256U> cgram{};
        std::uint64_t ppu_vram_revision{};
        std::int16_t scroll_x{};
        std::int16_t scroll_y{};
        bool valid{};
        bool bg1_valid{};
        bool panorama_valid{};
    };

    struct RenderItem {
        simulation::ObjectHandle handle{};
        std::array<std::int16_t, 3> position{};
        simulation::MatrixQ15 rotation_matrix{};
        std::int16_t world_x{};
        std::int16_t world_z{};
        friend bool operator==(const RenderItem&, const RenderItem&) = default;
    };

    [[nodiscard]] std::uint32_t ram_symbol(const char *name) const;
    [[nodiscard]] std::uint32_t fx_symbol(const char *name) const;
    [[nodiscard]] std::uint16_t colour_symbol(const char *name) const;
    [[nodiscard]] const assets::Shape* load_base_shape(
        std::uint16_t shape, std::uint16_t colour);
    [[nodiscard]] const assets::Shape* load_lod_shape(const assets::Shape& base,
        std::uint16_t pointer, std::uint16_t colour);
    [[nodiscard]] bool make_shape_cache_room(std::size_t bytes,
        const assets::Shape* protected_base = nullptr);

    const assets::RomImage& rom_;
    const assets::SymbolMap& symbols_;
    assets::ShapeDecoder decoder_;
    simulation::TrigTables trigonometry_;
    render::Framebuffer frame_{256U, 224U};
    render::Framebuffer wide_frame_{400U, 240U};
    // Native 400x240 world raster: wider view, rather than a scaled 256x224
    // texture. The software model renderer clips to these physical pixels.
    render::Framebuffer world_{400U, 240U};
    render::Framebuffer right_world_{400U, 240U};
    render::Framebuffer right_frame_{400U, 240U};
    std::uint8_t stereo_strength_{};
    bool last_draw_stereo_{};
    // Indexed world pixels are independent of PPU composition and CGRAM.
    // Key reuse from the quantized poses and view matrix actually consumed by
    // the rasterizer; interpolation alpha alone is not a pixel dependency.
    std::array<std::int32_t, 18U> world_cache_key_{};
    simulation::MatrixQ15 world_cache_view_{};
    std::vector<RenderItem> world_cache_render_items_{};
    std::uint64_t world_cache_scene_revision_{};
    std::uint64_t last_ppu_memory_revision_{};
    std::uint64_t world_cache_validation_us_{};
    std::uint64_t world_cache_snapshot_us_{};
    std::vector<simulation::ObjectHandle> world_cache_draw_order_{};
    std::vector<std::optional<simulation::GameObject>> world_cache_objects_{};
    std::array<simulation::ParticleState, simulation::kMaximumParticles>
        world_cache_particles_{};
    std::array<simulation::DustPoint, simulation::kMaximumDustPoints>
        world_cache_dust_{};
    bool world_cache_valid_{};
    std::uint64_t world_cache_hits_{};
    std::uint64_t world_cache_misses_{};
    std::uint64_t background_cache_hits_{};
    std::uint64_t background_cache_misses_{};
    DrawBreakdown last_draw_breakdown_{};
    render::Framebuffer ex_overlay_{256U, 224U};
    render::Framebuffer mode2_background_cache_{400U, 240U};
    std::array<Mode1BackgroundCache, 2U> mode1_caches_{};
    std::uint8_t mode1_next_victim_{};
    simulation::SnesPpuState mode2_background_ppu_{};
    std::array<std::uint16_t, 256U> mode2_background_cgram_{};
    std::uint64_t mode2_background_ppu_vram_revision_{};
    std::uint64_t mode2_background_scene_revision_{};
    std::int16_t mode2_background_x_{};
    std::int16_t mode2_background_y_{};
    std::int16_t background_scroll_x_{};
    std::int16_t background_scroll_y_{};
    bool mode2_background_valid_{};
    simulation::SnesPpuState last_ppu_{};
    simulation::GameFlowState last_flow_{};
    std::uint64_t last_scene_revision_{};
    bool last_frame_gpu_bg2_{};
    bool last_frame_wide_{};
    bool last_draw_valid_{};
    double last_interpolation_alpha_{1.0};
    timing::TransformSnapshot previous_camera_{};
    timing::TransformSnapshot current_camera_{};
    Bg2Motion bg2_motion_{};
    simulation::SnesPpuState presentation_ppu_{};
    render::ObjectSnapshotMap previous_objects_{};
    render::ObjectSnapshotMap current_objects_{};
    std::uint64_t snapshot_scene_revision_{};
    simulation::GameFlowState snapshot_flow_{};
    bool snapshots_ready_{};
    render::BackgroundRenderer backgrounds_;
    render::SpriteRenderer sprites_;
    render::Framebuffer corner_hud_{256U,224U};
    render::Framebuffer meter_hud_{256U,224U};
    render::Framebuffer top_hud_cache_{400U,240U};
    std::vector<NativeCoverageSpan> top_hud_spans_{};
    std::vector<NativeCoverageSpan> source_foreground_spans_{};
    SpriteSourceCache source_sprite_cache_{SpriteSourceCache::Selection::foreground};
    SpriteSourceCache hud_sprite_cache_{SpriteSourceCache::Selection::status_hud};
    std::uint64_t source_foreground_cache_hits_{}, source_foreground_cache_misses_{};
    std::uint64_t source_foreground_selective_hits_{}, top_hud_selective_hits_{};
    bool source_foreground_valid_{};
    bool source_foreground_paused_{};
    simulation::Experience source_foreground_experience_{};
    std::uint64_t top_hud_ppu_memory_revision_{};
    simulation::MeterState top_hud_meters_{};
    simulation::Experience top_hud_experience_{};
    std::uint8_t top_hud_object_select_{};
    bool top_hud_objects_enabled_{};
    bool top_hud_cache_valid_{};
    std::uint64_t top_hud_cache_hits_{};
    std::uint64_t top_hud_cache_misses_{};
    render::software_renderer_detail::Scratch model_scratch_;
    render::SoftwareRenderer models_;
    render::ParticleRenderer particles_;
    render::ScaledTextRenderer text_;
    bool show_top_hud_{true};
    render::DustRenderer dust_;
    struct CachedShape {
        assets::Shape shape;
        std::size_t bytes{};
        std::uint64_t last_use{};
    };
    std::unordered_map<std::uint32_t, CachedShape> shapes_;
    // Compact LODs inherit scale/material/bounds from their parent. They
    // cannot share the base cache or a key containing only the child address.
    std::unordered_map<std::uint64_t, CachedShape> lod_shapes_;
    std::size_t shape_cache_limit_{12U * 1024U * 1024U};
    std::size_t shape_cache_bytes_{};
    std::uint64_t shape_cache_clock_{}, shape_cache_insertions_{}, shape_cache_evictions_{};
    // Unusually large single assets still render without defeating the
    // retained-cache budget. At most one transient base and one LOD exist.
    std::optional<assets::Shape> transient_base_shape_, transient_lod_shape_;
    std::unordered_set<std::uint64_t> invalid_lods_;
    std::unordered_set<std::uint32_t> invalid_shapes_;
    std::vector<RenderItem> items_;
};

} // namespace starfox::platform_3ds
