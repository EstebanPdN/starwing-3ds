#pragma once
#include "mode1_bg2_atlas.hpp"
#include "mode1_layer_plan.hpp"

#include "starfox/assets/shape_decoder.hpp"
#include "starfox/render/background_renderer.hpp"
#include "starfox/render/dust_renderer.hpp"
#include "starfox/render/exterior_ship_visibility.hpp"
#include "starfox/render/object_snapshot.hpp"
#include "starfox/render/particle_renderer.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include "starfox/render/software_renderer.hpp"
#include "starfox/render/source_depth_tables.hpp"
#include "starfox/render/sprite_renderer.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "native_canvas.hpp"
#include "hud_layout.hpp"
#include "sprite_source_cache.hpp"
#include "bg3_atlas_cache.hpp"
#include "bg2_motion.hpp"
#include "native_engine/scene.hpp"

#include <cstdint>
#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Frame3ds builds each top-screen frame from the simulation: interpolated object snapshots and
// camera, models (software or recorded for PICA), dust, backgrounds and the top HUD. Used by
// game_3ds.cpp; host-tested by tests/frame_3ds_test.cpp and tests/stereo_3ds_test.cpp.

namespace starfox::platform_3ds {

// Cartridge foreground retains its 256x224 raster, centred at (72, 8).
// Gameplay geometry and scenery share the complete 400x240 top screen.
class Frame3ds {
public:
    // Time spent in the last draw(): stages in milliseconds and finer parts
    // in microseconds, measured with the ARM11 system tick.
    struct DrawBreakdown {
        float background_ms{};
        float world_ms{};
        float finish_ms{};
        std::uint32_t world_prepare_us{};
        std::uint32_t world_raster_us{};
        std::uint32_t world_composite_us{};
        std::uint32_t native_model_us{};
        std::uint32_t finalize_us{};
        std::uint32_t world_record_us{}, scene_compile_us{};
    };

    // Keeps references to `rom` and `symbols`, which must outlive the frame,
    // and resolves every symbol it reads once, here.
    Frame3ds(const assets::RomImage& rom, const assets::SymbolMap& symbols);
    // Record the world for the PICA native engine instead of rasterizing it:
    // Mode 2 gameplay drawn with gpu_bg2, and Mode 1 levels when `mode1` is
    // set. Stereo frames always rasterize. A change invalidates the last draw.
    void set_native_engine(bool enabled, bool mode1=false) noexcept {
        if (native_engine_enabled_ != enabled || native_mode1_enabled_ != mode1)
            last_draw_valid_ = false;
        native_engine_enabled_=enabled; native_mode1_enabled_=mode1;
    }
    // What the last PICA world was compiled from. The frame records it
    // packed; the full commands are rebuilt here, for whoever asks.
    const render::RasterCommands& native_commands() const {
        native_commands_.unpack();
        return native_commands_;
    }
    // The 400x240 Mode 1 background for Display3ds::present_native() and the
    // revision that names its contents.
    const render::Framebuffer& native_background() const noexcept { return native_background_; }
    std::uint32_t native_background_revision() const noexcept { return native_background_revision_; }
    // The last draw's compiled PICA world; null when that draw rasterized
    // the world in software or the scene overflowed the GPU batch.
    const native_engine::Scene* native_scene() const noexcept { return native_scene_active_ ? &native_scene_ : nullptr; }
    // Let Mode 1 levels go to Display3ds::present_mode1_layers() as a layer
    // plan (mode1_layers(), null when the last draw composed them itself)
    // instead of a CPU-composed background.
    void set_retained_mode1_layers(bool enabled) noexcept {
        if (retained_mode1_enabled_!=enabled) last_draw_valid_=false;
        retained_mode1_enabled_=enabled;
    }
    [[nodiscard]] const Mode1LayerPlan* mode1_layers() const noexcept {
        return retained_mode1_active_ ? &mode1_layer_plan_ : nullptr;
    }
    // Per-eye stereo offset in pixels (the eyes move in opposite directions),
    // clamped to 8; 0 turns stereo off. Only Mode 2 gameplay draws a
    // right-eye frame (stereo_active() after that draw).
    void set_stereo_strength(std::uint8_t strength) noexcept;
    [[nodiscard]] bool stereo_active() const noexcept { return last_draw_stereo_; }
    [[nodiscard]] std::uint8_t stereo_strength() const noexcept { return stereo_strength_; }
    // The last draw's right-eye frame, or last_frame() when it had none.
    [[nodiscard]] const render::Framebuffer& right_frame() const noexcept {
        return last_draw_stereo_ ? right_frame_ : last_frame();
    }
    // Lays the top HUD out for the full 400x240 view (true) or the 4:3 view.
    void set_wide_view(bool wide) noexcept {
        if (wide_view_ != wide) {
            wide_view_ = wide;
            top_hud_cache_valid_ = last_draw_valid_ = false;
        }
    }
    void set_top_hud_visible(bool visible) noexcept {
        if(show_top_hud_!=visible) {
            show_top_hud_=visible;
            last_draw_valid_=false;
        }
    }
    // Builds the frame for one presentation and returns the 400x240 wide
    // frame, valid until the next draw. gpu_bg2: Display3ds draws BG2 in
    // Mode 2 gameplay, so the frame holds only the layers above it.
    // interpolation_alpha (0..1) places camera, objects and BG2 between the
    // previous and the latest source tick. prepared_background is the
    // presentation_ppu() snapshot a BG2 worker may be reading.
    [[nodiscard]] const render::Framebuffer& draw(
        const simulation::GameSimulation& game, bool gpu_bg2 = false,
        double interpolation_alpha = 1.0,
        const simulation::SnesPpuState* prepared_background = nullptr);
    // Whether the last frame can be shown again: same gpu_bg2 mode, alpha,
    // flow, scene revision, PPU memory revision and background state as the
    // last draw().
    [[nodiscard]] bool can_reuse_presentation(
        const simulation::GameSimulation& game, bool gpu_bg2,
        double interpolation_alpha = 1.0) const;
    // Records the presentation endpoints after a source tick: camera, object
    // poses and BG2 motion. A scene cut, flow change or camera jump makes
    // both endpoints equal, so nothing interpolates across it.
    void capture_after_tick(const simulation::GameSimulation& game);
    [[nodiscard]] const render::Framebuffer& last_frame() const noexcept {
        return last_frame_wide_ ? wide_frame_ : frame_;
    }
    // Names the contents of the wide frame: it changes whenever draw()
    // writes that frame, and only then. Never zero.
    [[nodiscard]] std::uint32_t wide_frame_revision() const noexcept {
        return wide_foreground_revision_;
    }
    // Decode the stage's currently active geometry while the source display
    // is black, before the first visible gameplay presentation. Returns the
    // number of shapes added to the cache.
    [[nodiscard]] unsigned preload_active_shapes(
        const simulation::GameSimulation& game);
    struct PreloadStats { unsigned candidates{}, decoded{}, rejected{}; std::size_t bytes{}; bool limited{}; };
    using PreloadProgress = bool (*)(void*, unsigned, unsigned);
    // Sets the shape cache budget in bytes and decodes every symbolized
    // model with its LOD, shadow and flash-colour variants while they fit
    // (`limited` when some did not), then sizes the model scratch for the
    // largest one. progress(context, done, total) returning false stops it.
    [[nodiscard]] PreloadStats preload_catalog(std::size_t budget,
        PreloadProgress progress = nullptr, void* context = nullptr);
    // The BG2 scroll the last draw() used.
    [[nodiscard]] std::int16_t background_scroll_x() const noexcept {
        return background_scroll_x_;
    }
    [[nodiscard]] std::int16_t background_scroll_y() const noexcept {
        return background_scroll_y_;
    }
    // BG2 scroll for a presentation at `interpolation_alpha`: the PPU
    // registers in Mode 3, Bg2Motion in Mode 2 gameplay while the PPU still
    // matches its captured endpoint, else the BG2XSCROLL/BG2SCROLL words.
    [[nodiscard]] std::pair<std::int16_t, std::int16_t>
    current_background_scroll(const simulation::GameSimulation& game,
        double interpolation_alpha = 1.0) const;
    // Prepare once before the BG2 worker reads this immutable snapshot. Pass
    // it to draw() so that CPU rendering cannot rewrite the worker's input.
    [[nodiscard]] const simulation::SnesPpuState& presentation_ppu(
        const simulation::GameSimulation& game, double interpolation_alpha);
    // Cache counters, timings and sizes for the diagnostics reports.
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
            sizeof(source_sprite_cache_) + sizeof(hud_sprite_cache_) + sizeof(mode1_empty_sprite_cache_),
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
        std::size_t mode1_layer_plan_capacity_bytes{};
    };
    // On-demand dump inventory: exact vector payload capacities, not an
    // allocator/heap total. Map node allocations and shape data are separate.
    [[nodiscard]] StorageStats storage_stats() const noexcept;

private:
    bool native_engine_enabled_{}, native_mode1_enabled_{}, native_scene_active_{}, last_native_world_{};
    mutable render::RasterCommands native_commands_;
    render::Framebuffer native_background_{0,0}, empty_native_{0,0};
    std::uint32_t native_background_revision_{1U};
    bool native_mode1_layers_valid_{};
    bool retained_mode1_enabled_{}, retained_mode1_active_{}, last_retained_mode1_{};
    Mode1LayerPlan mode1_layer_plan_;
    SpriteSourceCache mode1_empty_sprite_cache_{SpriteSourceCache::Selection::foreground};
    std::uint32_t panorama_atlas_revision_{1U};
    std::int16_t native_mode1_scroll_x_{}, native_mode1_scroll_y_{};
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
        Bg3AtlasSourceCache atlas_source{};
        std::array<std::uint16_t, 256U> cgram{};
        std::uint64_t ppu_vram_revision{};
        std::int16_t scroll_x{};
        std::int16_t scroll_y{};
        bool valid{};
        bool bg1_valid{};
        bool panorama_valid{};
        bool bg2_raster_valid{};
        std::uint32_t atlas_revision{};
    };

    struct RenderItem {
        simulation::ObjectHandle handle{};
        std::array<std::int16_t, 3> position{};
        simulation::MatrixQ15 rotation_matrix{};
        std::int16_t world_x{};
        std::int16_t world_z{};
        // The live object, valid for the draw that built this item.
        const simulation::GameObject* object{};
    };

    // A symbol's first address in work RAM (ram_symbol: bank 00 or 7E), in
    // Super FX RAM (fx_symbol: bank 70) or in ROM (colour_symbol: the 16-bit
    // address of a colour table); 0 when there is none.
    [[nodiscard]] std::uint32_t ram_symbol(const char *name) const;
    [[nodiscard]] std::uint32_t fx_symbol(const char *name) const;
    [[nodiscard]] std::uint16_t colour_symbol(const char *name) const;
    // Decoded shape from the cache, decoding it on a miss; null when it does
    // not decode. A shape that cannot fit the budget becomes the transient,
    // valid until the next oversized decode of its kind or the next draw.
    [[nodiscard]] const assets::Shape* load_base_shape(
        std::uint16_t shape, std::uint16_t colour, std::uint16_t minimum_colours = 0);
    [[nodiscard]] const assets::Shape* load_lod_shape(const assets::Shape& base,
        std::uint16_t pointer, std::uint16_t colour, std::uint16_t minimum_colours = 0);
    // Evicts least recently used shapes, never protected_base, until `bytes`
    // more fit the budget; false when they cannot.
    [[nodiscard]] bool make_shape_cache_room(std::size_t bytes,
        const assets::Shape* protected_base = nullptr);

    const assets::RomImage& rom_;
    const assets::SymbolMap& symbols_;
    assets::ShapeDecoder decoder_;
    simulation::TrigTables trigonometry_;
    render::Framebuffer frame_{256U, 224U};
    render::Framebuffer wide_frame_{400U, 240U};
    // The wide frame holds the 2D layer of a PICA world, composed with or
    // without the top HUD.
    bool wide_foreground_valid_{};
    bool wide_foreground_top_hud_{};
    std::uint32_t wide_foreground_revision_{1U};
    // Native 400x240 world raster: wider view, rather than a scaled 256x224
    // texture. The software model renderer clips to these physical pixels.
    render::Framebuffer world_{400U, 240U};
    render::Framebuffer right_world_{400U, 240U};
    render::Framebuffer right_frame_{400U, 240U};
    std::uint8_t stereo_strength_{};
    bool last_draw_stereo_{};
    // The world is recorded again at every draw. A whole-state cache (a copy
    // of every object, particle and dust point per frame, compared at the
    // next) would match only a repeated presentation of one instant, which
    // the running game loop does not produce, and a match would keep exactly
    // what recording the same inputs again yields.
    // world_cache_misses_ counts the recordings; hits stay zero.
    std::uint64_t last_ppu_memory_revision_{};
    std::uint64_t world_cache_validation_us_{};
    std::uint64_t world_cache_snapshot_us_{};
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
    // Presentation endpoints of every object slot, indexed by handle. Two
    // captures are kept; a capture overwrites the older one in place, so a
    // source tick allocates nothing and a draw looks nothing up by hash.
    struct ObjectSnapshots {
        static constexpr std::size_t slots = simulation::kMaximumObjects + 1U;
        std::array<render::ObjectPresentationSnapshot, slots> entries{};
        std::array<bool, slots> present{};
        std::uint32_t count{};
        [[nodiscard]] const render::ObjectPresentationSnapshot* find(
            simulation::ObjectHandle handle) const noexcept {
            return handle < slots && present[handle] ? &entries[handle] : nullptr;
        }
    };
    std::array<ObjectSnapshots, 2U> object_snapshots_{};
    // object_snapshots_[current_objects_index_] is the latest capture. The
    // other is the one before, unless a scene cut made both ends the same.
    std::uint8_t current_objects_index_{};
    bool previous_objects_are_current_{true};
    [[nodiscard]] const ObjectSnapshots& current_objects() const noexcept {
        return object_snapshots_[current_objects_index_];
    }
    [[nodiscard]] const ObjectSnapshots& previous_objects() const noexcept {
        return object_snapshots_[previous_objects_are_current_
            ? current_objects_index_ : 1U - current_objects_index_];
    }
    std::uint64_t snapshot_scene_revision_{};
    simulation::GameFlowState snapshot_flow_{};
    bool snapshots_ready_{};
    Mode1Bg2Atlas bg2_mode1_atlas_;
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
    // The top HUD is retained by group: lives, boss health, shield stack and
    // bombs/boost stack. A group keeps the two source crops it was scaled
    // from and where they landed in top_hud_cache_, so a meter that moves
    // rewrites that group's rectangle and not the whole 400x240 layer.
    struct TopHudGroup {
        HudRect upper{}, bar{};
        HudRect upper_target{}, bar_target{};
    };
    std::array<TopHudGroup, 4U> top_hud_groups_{};
    // What meter_hud_ holds, in paint order.
    render::MeterRectangles top_hud_meter_rectangles_{};
    // Groups that overlap need the complete ordered rebuild.
    bool top_hud_disjoint_{};
    // Where the last refresh rewrote top_hud_cache_.
    static constexpr std::size_t top_hud_damage_capacity = 12U;
    std::array<HudRect, top_hud_damage_capacity> top_hud_damage_{};
    std::size_t top_hud_damage_count_{};
    bool top_hud_damage_all_{};
    // Mode 2 over a PICA world: the wide frame is empty but for
    // source_foreground_spans_ and the top HUD.
    bool wide_source_layer_{};
    [[nodiscard]] bool refresh_top_hud(const simulation::SnesPpuState& ppu,
        const simulation::MeterState& meters, simulation::Experience experience,
        std::uint64_t ppu_memory_revision, std::uint64_t ppu_vram_revision);
    void add_top_hud_damage(HudRect area) noexcept;
    void scan_top_hud_spans();
    void refresh_top_hud_spans();
    void patch_wide_top_hud(bool source_foreground) noexcept;
    void erase_wide_source_layer(bool hud_damage) noexcept;
    void draw_wide_source_layer() noexcept;
    void draw_wide_top_hud() noexcept;
    std::uint64_t top_hud_cache_hits_{};
    std::uint64_t top_hud_cache_misses_{};
    render::software_renderer_detail::Scratch model_scratch_;
    render::SoftwareRenderer models_;
    render::ParticleRenderer particles_;
    render::ScaledTextRenderer text_;
    bool show_top_hud_{true};
    bool wide_view_{true};
    render::DustRenderer dust_;
    struct CachedShape {
        assets::Shape shape;
        std::size_t bytes{};
        std::uint64_t last_use{};
    };
    std::unordered_map<std::uint64_t, CachedShape> shapes_;
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
    std::unordered_set<std::uint64_t> invalid_shapes_;
    std::vector<RenderItem> items_;

    // ---- World path: values resolved once, or once per source tick ----
    // The symbol map is immutable for the session. Every address the frame
    // reads by name is looked up at construction, never per draw.
    struct ResolvedSymbols {
        std::uint32_t view_pos_x{}, view_pos_y{}, view_pos_z{};
        std::uint32_t view_rot_x{}, view_rot_y{}, view_rot_z{};
        std::uint32_t bg2_x_scroll{}, bg2_scroll{};
        std::uint32_t game_frame{}, shadow_height{}, player_fly_mode{};
        std::uint32_t vanish_x{}, vanish_y{}, depth_colours{}, depth_thresholds{};
        std::uint32_t flash_player_strategy{}, depth_tables{};
        std::uint16_t special_colour{}, red_colour{}, white_colour{};
    };
    ResolvedSymbols resolved_{};
    render::ExteriorShipVisibility exterior_visibility_;
    // Rotation matrices of the two camera endpoints (pure functions of
    // previous_camera_ and current_camera_), refreshed at each capture.
    simulation::MatrixQ15 previous_view_matrix_{}, current_view_matrix_{};
    // The scene's depth-cue tables are ROM constants selected by three
    // words; read them from the ROM again only when a word changes.
    std::optional<render::SourceDepthTables> depth_tables_;
    std::uint32_t depth_tables_address_{};
    std::uint16_t depth_tables_thresholds_{}, depth_tables_colours_{};
    // What an object slot last resolved its model to. A binding is good
    // while its key is asked again and the shape caches have not changed;
    // then the draw skips every hash lookup and only stamps the entry's use.
    struct ShapeBinding {
        std::uint64_t key{}, generation{};
        CachedShape* entry{};
    };
    static constexpr std::size_t binding_slots = simulation::kMaximumObjects + 1U;
    std::array<ShapeBinding, binding_slots> base_bindings_{};
    std::array<ShapeBinding, binding_slots> lod_bindings_{};
    std::array<ShapeBinding, binding_slots> shadow_bindings_{};
    // Advanced by every insertion into, or removal from, a shape cache or a
    // rejected-shape set. Never zero.
    std::uint64_t shape_cache_generation_{1U};
    [[nodiscard]] const assets::Shape* resolve_base_shape(std::uint16_t shape,
        std::uint16_t colour, std::uint16_t minimum_colours, CachedShape** entry);
    [[nodiscard]] const assets::Shape* resolve_lod_shape(const assets::Shape& base,
        std::uint16_t pointer, std::uint16_t colour, std::uint16_t minimum_colours,
        CachedShape** entry);
    [[nodiscard]] const assets::Shape* bound_base_shape(ShapeBinding& binding,
        std::uint16_t shape, std::uint16_t colour, std::uint16_t minimum_colours);
    [[nodiscard]] const assets::Shape* bound_lod_shape(ShapeBinding& binding,
        const assets::Shape& base, std::uint16_t pointer, std::uint16_t colour,
        std::uint16_t minimum_colours);
    // presentation_ppu_ mirrors the live PPU. Its memory arrays are copied
    // again only when the simulation's revision tokens say they changed.
    std::uint64_t presentation_ppu_memory_revision_{}, presentation_ppu_vram_revision_{};
    bool presentation_ppu_synced_{};
};

} // namespace starfox::platform_3ds
