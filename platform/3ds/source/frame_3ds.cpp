#include "frame_3ds.hpp"
#include "starfox/render/gpu_raster.hpp"
#include "hud_layout.hpp"
#include "shape_memory.hpp"
#include "native_canvas.hpp"
#include "stereo_3ds.hpp"
#include "presentation_3ds.hpp"

#include "starfox/render/palette.hpp"

#include <3ds.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#ifdef STARWING_DEEP_PROFILE
#include <cstdio>
#endif

namespace starfox::platform_3ds {
namespace {

render::RenderSettings model_settings() {
    render::RenderSettings settings;
    settings.colour_index_base = 7U * 16U;
    return settings;
}

bool same_bg2_tiles(const simulation::SnesPpuState& a,
    const simulation::SnesPpuState& b) noexcept {
    return a.background_mode == b.background_mode
        && a.main_screen == b.main_screen
        && a.mosaic == b.mosaic
        && a.bg2_tile_size_16 == b.bg2_tile_size_16
        && a.bg2_character_base == b.bg2_character_base
        && a.bg2_screen_base == b.bg2_screen_base
        && a.bg2_screen_size == b.bg2_screen_size
        && a.bg2_scroll_x == b.bg2_scroll_x
        && a.bg2_scroll_y == b.bg2_scroll_y
        && a.bg2_vertical_offsets_enabled == b.bg2_vertical_offsets_enabled
        && a.bg2_horizontal_offsets_enabled == b.bg2_horizontal_offsets_enabled
        && (!a.bg2_horizontal_offsets_enabled
            || a.bg2_horizontal_offsets == b.bg2_horizontal_offsets)
        && a.bg2_scanline_scroll_enabled == b.bg2_scanline_scroll_enabled
        && (!a.bg2_scanline_scroll_enabled
            || a.bg2_scanline_scroll_y == b.bg2_scanline_scroll_y)
        && a.tunnel_scene == b.tunnel_scene;
}

#ifdef STARWING_DEEP_PROFILE
bool same_mode1_tiles(const simulation::SnesPpuState& a,
    const simulation::SnesPpuState& b) noexcept {
    return same_bg2_tiles(a, b)
        && a.bg1_tile_size_16 == b.bg1_tile_size_16
        && a.bg1_character_base == b.bg1_character_base
        && a.bg1_screen_base == b.bg1_screen_base
        && a.bg1_screen_size == b.bg1_screen_size
        && a.bg1_scroll_x == b.bg1_scroll_x
        && a.bg1_scroll_y == b.bg1_scroll_y
        && a.bg3_tile_size_16 == b.bg3_tile_size_16
        && a.bg3_character_base == b.bg3_character_base
        && a.bg3_screen_base == b.bg3_screen_base
        && a.bg3_screen_size == b.bg3_screen_size
        && a.bg3_scroll_x == b.bg3_scroll_x
        && a.bg3_scroll_y == b.bg3_scroll_y;
}
#endif

bool same_mode1_bg1(const simulation::SnesPpuState& a,
    const simulation::SnesPpuState& b) noexcept {
    return a.background_mode == b.background_mode
        && (a.main_screen & 0x01U) == (b.main_screen & 0x01U)
        && a.mosaic == b.mosaic
        && a.bg1_tile_size_16 == b.bg1_tile_size_16
        && a.bg1_character_base == b.bg1_character_base
        && a.bg1_screen_base == b.bg1_screen_base
        && a.bg1_screen_size == b.bg1_screen_size
        && a.bg1_scroll_x == b.bg1_scroll_x
        && a.bg1_scroll_y == b.bg1_scroll_y;
}

bool same_mode1_bg3_tiles(const simulation::SnesPpuState& a,
    const simulation::SnesPpuState& b) noexcept {
    return a.background_mode == b.background_mode
        && (a.main_screen & 0x04U) == (b.main_screen & 0x04U)
        && a.bg3_tile_size_16 == b.bg3_tile_size_16
        && a.bg3_character_base == b.bg3_character_base
        && a.bg3_screen_base == b.bg3_screen_base
        && a.bg3_screen_size == b.bg3_screen_size;
}

bool same_mode1_bg3(const simulation::SnesPpuState& a,
    const simulation::SnesPpuState& b) noexcept {
    return same_mode1_bg3_tiles(a, b)
        && a.mosaic == b.mosaic
        && a.bg3_scroll_x == b.bg3_scroll_x
        && a.bg3_scroll_y == b.bg3_scroll_y;
}

// BG2 scroll/HDMA can change on every 60 Hz presentation even when the
// Super FX world and OBJ foreground are unchanged. PICA owns BG2 in this
// path, so those registers must not force a CPU foreground rebuild.
bool same_mode2_foreground(const simulation::SnesPpuState& a,
    const simulation::SnesPpuState& b) noexcept {
    return a.background_mode == b.background_mode
        && a.bg3_high_priority == b.bg3_high_priority
        && a.bg1_tile_size_16 == b.bg1_tile_size_16
        && a.bg3_tile_size_16 == b.bg3_tile_size_16
        && a.bg4_tile_size_16 == b.bg4_tile_size_16
        && a.mosaic == b.mosaic && a.object_select == b.object_select
        && a.bg1_character_base == b.bg1_character_base
        && a.bg1_screen_base == b.bg1_screen_base
        && a.bg1_screen_size == b.bg1_screen_size
        && a.bg1_scroll_x == b.bg1_scroll_x
        && a.bg1_scroll_y == b.bg1_scroll_y
        && a.bg3_character_base == b.bg3_character_base
        && a.bg3_screen_base == b.bg3_screen_base
        && a.bg3_screen_size == b.bg3_screen_size
        && a.bg3_scroll_x == b.bg3_scroll_x
        && a.bg3_scroll_y == b.bg3_scroll_y
        && a.main_screen == b.main_screen
        && a.tunnel_scene == b.tunnel_scene;
}

bool same_ppu_metadata(const simulation::SnesPpuState& a,
    const simulation::SnesPpuState& b) noexcept {
    return a.background_mode == b.background_mode
        && a.bg3_high_priority == b.bg3_high_priority
        && a.bg1_tile_size_16 == b.bg1_tile_size_16
        && a.bg2_tile_size_16 == b.bg2_tile_size_16
        && a.bg3_tile_size_16 == b.bg3_tile_size_16
        && a.bg4_tile_size_16 == b.bg4_tile_size_16
        && a.mosaic == b.mosaic && a.object_select == b.object_select
        && a.bg1_character_base == b.bg1_character_base
        && a.bg1_screen_base == b.bg1_screen_base
        && a.bg1_screen_size == b.bg1_screen_size
        && a.bg1_scroll_x == b.bg1_scroll_x
        && a.bg1_scroll_y == b.bg1_scroll_y
        && a.bg2_character_base == b.bg2_character_base
        && a.bg2_screen_base == b.bg2_screen_base
        && a.bg2_screen_size == b.bg2_screen_size
        && a.bg2_scroll_x == b.bg2_scroll_x
        && a.bg2_scroll_y == b.bg2_scroll_y
        && a.bg3_screen_base == b.bg3_screen_base
        && a.bg3_screen_size == b.bg3_screen_size
        && a.bg3_character_base == b.bg3_character_base
        && a.bg3_scroll_x == b.bg3_scroll_x
        && a.bg3_scroll_y == b.bg3_scroll_y
        && a.main_screen == b.main_screen
        && a.bg2_vertical_offsets_enabled == b.bg2_vertical_offsets_enabled
        && a.bg2_horizontal_offsets == b.bg2_horizontal_offsets
        && a.bg2_horizontal_offsets_enabled == b.bg2_horizontal_offsets_enabled
        && a.bg2_scanline_scroll_y == b.bg2_scanline_scroll_y
        && a.bg2_scanline_scroll_enabled == b.bg2_scanline_scroll_enabled
        && a.tunnel_scene == b.tunnel_scene;
}

void capture_ppu_metadata(simulation::SnesPpuState& destination,
    const simulation::SnesPpuState& source) noexcept {
    destination.background_mode = source.background_mode;
    destination.bg3_high_priority = source.bg3_high_priority;
    destination.bg1_tile_size_16 = source.bg1_tile_size_16;
    destination.bg2_tile_size_16 = source.bg2_tile_size_16;
    destination.bg3_tile_size_16 = source.bg3_tile_size_16;
    destination.bg4_tile_size_16 = source.bg4_tile_size_16;
    destination.mosaic = source.mosaic;
    destination.object_select = source.object_select;
    destination.bg1_character_base = source.bg1_character_base;
    destination.bg1_screen_base = source.bg1_screen_base;
    destination.bg1_screen_size = source.bg1_screen_size;
    destination.bg1_scroll_x = source.bg1_scroll_x;
    destination.bg1_scroll_y = source.bg1_scroll_y;
    destination.bg2_character_base = source.bg2_character_base;
    destination.bg2_screen_base = source.bg2_screen_base;
    destination.bg2_screen_size = source.bg2_screen_size;
    destination.bg2_scroll_x = source.bg2_scroll_x;
    destination.bg2_scroll_y = source.bg2_scroll_y;
    destination.bg3_screen_base = source.bg3_screen_base;
    destination.bg3_screen_size = source.bg3_screen_size;
    destination.bg3_character_base = source.bg3_character_base;
    destination.bg3_scroll_x = source.bg3_scroll_x;
    destination.bg3_scroll_y = source.bg3_scroll_y;
    destination.main_screen = source.main_screen;
    destination.bg2_vertical_offsets_enabled = source.bg2_vertical_offsets_enabled;
    destination.bg2_horizontal_offsets = source.bg2_horizontal_offsets;
    destination.bg2_horizontal_offsets_enabled = source.bg2_horizontal_offsets_enabled;
    destination.bg2_scanline_scroll_y = source.bg2_scanline_scroll_y;
    destination.bg2_scanline_scroll_enabled = source.bg2_scanline_scroll_enabled;
    destination.tunnel_scene = source.tunnel_scene;
}

} // namespace

Frame3ds::Frame3ds(const assets::RomImage& rom,
    const assets::SymbolMap& symbols)
    : rom_(rom), symbols_(symbols), decoder_(rom, symbols),
      trigonometry_(simulation::TrigTables::load(rom, symbols)),
      models_(model_settings()), text_(rom, symbols), dust_(rom, symbols) {
    top_hud_spans_.reserve(512U);
    source_foreground_spans_.reserve(256U);
    items_.reserve(simulation::kMaximumObjects);
    world_cache_objects_.reserve(simulation::kMaximumObjects);
    world_cache_render_items_.reserve(simulation::kMaximumObjects);
}

void Frame3ds::capture_after_tick(const simulation::GameSimulation& game) {
    const auto& map = game.map();
    const auto camera = timing::TransformSnapshot{
        static_cast<std::int16_t>(map.read_native_word(ram_symbol("VIEWPOSX"))),
        static_cast<std::int16_t>(map.read_native_word(ram_symbol("VIEWPOSY"))),
        static_cast<std::int16_t>(map.read_native_word(ram_symbol("VIEWPOSZ"))),
        map.read_native_word(ram_symbol("VIEWROTXW")),
        map.read_native_word(ram_symbol("VIEWROTYW")),
        map.read_native_word(ram_symbol("VIEWROTZW"))};
    auto objects = render::capture_object_snapshots(
        game.objects(), trigonometry_);
    const bool scene_cut = !snapshots_ready_
        || game.scene_revision() != snapshot_scene_revision_
        || game.flow_state() != snapshot_flow_
        || timing::camera_transform_is_discontinuous(current_camera_, camera);
    bg2_motion_.capture(map.ppu_state(),
        static_cast<std::int16_t>(map.peek_ram_word(ram_symbol("BG2XSCROLL")).value_or(0)),
        static_cast<std::int16_t>(map.peek_ram_word(ram_symbol("BG2SCROLL")).value_or(0)),
        map.background(), scene_cut);
    if (!snapshots_ready_) {
        current_camera_ = previous_camera_ = camera;
        current_objects_ = objects;
        previous_objects_ = std::move(objects);
        snapshots_ready_ = true;
    } else {
        previous_camera_ = current_camera_;
        current_camera_ = camera;
        previous_objects_ = std::move(current_objects_);
        current_objects_ = std::move(objects);
        if (game.scene_revision() != snapshot_scene_revision_
            || game.flow_state() != snapshot_flow_
            || timing::camera_transform_is_discontinuous(
                previous_camera_, current_camera_)) {
            previous_camera_ = current_camera_;
            previous_objects_ = current_objects_;
        }
    }
    snapshot_scene_revision_ = game.scene_revision();
    snapshot_flow_ = game.flow_state();
}

bool Frame3ds::make_shape_cache_room(std::size_t bytes,
    const assets::Shape* protected_base) {
    if (bytes > shape_cache_limit_) return false;
    while (shape_cache_bytes_ > shape_cache_limit_ - bytes) {
        auto base_victim = shapes_.end();
        auto lod_victim = lod_shapes_.end();
        auto oldest = std::numeric_limits<std::uint64_t>::max();
        for (auto it = shapes_.begin(); it != shapes_.end(); ++it) {
            if (&it->second.shape != protected_base && it->second.last_use <= oldest) {
                oldest = it->second.last_use;
                base_victim = it;
            }
        }
        for (auto it = lod_shapes_.begin(); it != lod_shapes_.end(); ++it) {
            if (it->second.last_use <= oldest) {
                oldest = it->second.last_use;
                lod_victim = it;
            }
        }
        if (lod_victim != lod_shapes_.end()) {
            shape_cache_bytes_ -= lod_victim->second.bytes;
            lod_shapes_.erase(lod_victim);
        } else if (base_victim != shapes_.end()) {
            shape_cache_bytes_ -= base_victim->second.bytes;
            shapes_.erase(base_victim);
        } else return false;
        ++shape_cache_evictions_;
    }
    return true;
}

const assets::Shape* Frame3ds::load_base_shape(
    std::uint16_t shape, std::uint16_t colour) {
    const auto key = (static_cast<std::uint32_t>(shape) << 16U) | colour;
    if (invalid_shapes_.contains(key)) return nullptr;
    auto found = shapes_.find(key);
    if (found == shapes_.end() && colour != 0U) {
        const auto original = shapes_.find(std::uint32_t{shape} << 16U);
        if (original != shapes_.end() && original->second.shape.header.colour_pointer == colour) {
            original->second.last_use = ++shape_cache_clock_;
            return &original->second.shape;
        }
    }
    if (found == shapes_.end()) {
        try {
            auto decoded = decoder_.decode(shape, {}, colour);
            const auto bytes = shape_storage_bytes(decoded);
            if (!make_shape_cache_room(bytes)) {
                transient_base_shape_ = std::move(decoded);
                return &*transient_base_shape_;
            }
            found = shapes_.emplace(key, CachedShape{std::move(decoded), bytes,
                ++shape_cache_clock_}).first;
            shape_cache_bytes_ += bytes;
            ++shape_cache_insertions_;
        } catch (const std::exception&) {
            if (invalid_shapes_.size() >= 4096U) invalid_shapes_.clear();
            invalid_shapes_.insert(key);
            return nullptr;
        }
    }
    // References survive unordered_map rehashes when a LOD is inserted.
    found->second.last_use = ++shape_cache_clock_;
    return &found->second.shape;
}

const assets::Shape* Frame3ds::load_lod_shape(const assets::Shape& base,
    std::uint16_t pointer, std::uint16_t colour) {
    if (pointer == static_cast<std::uint16_t>(base.header.address)) return &base;
    const auto material = colour != 0U ? colour : base.header.colour_pointer;
    const auto key = (std::uint64_t{base.header.address} << 32U)
        | (std::uint64_t{pointer} << 16U) | material;
    if (invalid_lods_.contains(key)) return nullptr;
    auto found = lod_shapes_.find(key);
    if (found == lod_shapes_.end()) {
        try {
            auto decoded = decoder_.decode_lod(base.header, pointer, colour);
            const auto bytes = shape_storage_bytes(decoded);
            // The caller may use base again after drawing this LOD. Other
            // unordered_map entries can be evicted without moving base.
            if (!make_shape_cache_room(bytes, &base)) {
                transient_lod_shape_ = std::move(decoded);
                return &*transient_lod_shape_;
            }
            found = lod_shapes_.emplace(key, CachedShape{std::move(decoded), bytes,
                ++shape_cache_clock_}).first;
            shape_cache_bytes_ += bytes;
            ++shape_cache_insertions_;
        } catch (const std::exception&) {
            if (invalid_lods_.size() >= 4096U) invalid_lods_.clear();
            invalid_lods_.insert(key);
            return nullptr;
        }
    }
    found->second.last_use = ++shape_cache_clock_;
    return &found->second.shape;
}

Frame3ds::PreloadStats Frame3ds::preload_catalog(std::size_t budget,
    PreloadProgress progress, void* context) {
    PreloadStats stats;
    shape_cache_limit_ = budget;
    static_cast<void>(make_shape_cache_room(0U));
    std::vector<std::uint16_t> candidates;
    for (const auto& [name, addresses] : symbols_.entries()) {
        static_cast<void>(name);
        for (auto address : addresses)
            if (address >= 0x8000U && address <= 0xffffU)
                candidates.push_back(static_cast<std::uint16_t>(address));
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    stats.candidates = static_cast<unsigned>(candidates.size());
    stats.bytes = shape_cache_bytes_;
    const auto retain = [&](auto& cache, auto key, assets::Shape&& shape) {
        const auto bytes = shape_storage_bytes(shape);
        if (stats.bytes > budget || bytes > budget - stats.bytes) {
            stats.limited = true;
            return false;
        }
        cache.emplace(key, CachedShape{std::move(shape), bytes, ++shape_cache_clock_});
        stats.bytes += bytes;
        shape_cache_bytes_ += bytes;
        ++shape_cache_insertions_;
        ++stats.decoded;
        return true;
    };
    // First cover every full model with its authored material. Compact LODs
    // must be decoded from their parent; a standalone decode loses scale.
    unsigned visited = 0U;
    std::vector<std::uint16_t> bases;
    for (auto address : candidates) {
        if (progress && !progress(context, visited++, stats.candidates)) break;
        const auto key = std::uint32_t{address} << 16U;
        if (shapes_.contains(key)) { bases.push_back(address); continue; }
        if (!decoder_.looks_like_shape_header(address)) continue;
        try {
            auto shape = decoder_.decode(address);
            if (shape.header.compact) continue;
            if (retain(shapes_, key, std::move(shape))) bases.push_back(address);
        } catch (const std::exception&) { ++stats.rejected; }
    }
    // Preload distance and shadow variants, followed by the three source
    // flash/damage colour tables. Runtime demand loading remains mandatory
    // for unsymbolized assets or when a console reaches its memory budget.
    const std::array<std::uint16_t, 4> colours{
        0U, colour_symbol("ID_1_C"), colour_symbol("RED_C"), colour_symbol("WHITE_C")};
    visited = 0U;
    for (auto colour : colours) for (auto address : bases) {
        if (progress && !progress(context, visited++,
                static_cast<unsigned>(bases.size() * colours.size()))) return stats;
        const auto key = (std::uint32_t{address} << 16U) | colour;
        const auto& original = shapes_.at(std::uint32_t{address} << 16U).shape;
        const auto* base = &original;
        if (colour != 0U && colour != original.header.colour_pointer) {
            auto found = shapes_.find(key);
            if (found == shapes_.end()) {
                try {
                    if (!retain(shapes_, key, decoder_.decode(address, {}, colour))) continue;
                    found = shapes_.find(key);
                } catch (const std::exception&) { ++stats.rejected; continue; }
            }
            base = &found->second.shape;
        }
        for (auto pointer : {base->header.lod1_pointer, base->header.lod2_pointer,
                 base->header.lod3_pointer, base->header.shadow_pointer}) {
            if (pointer == address) continue;
            const auto material = colour ? colour : base->header.colour_pointer;
            const auto lod_key = (std::uint64_t{address} << 32U)
                | (std::uint64_t{pointer} << 16U) | material;
            if (lod_shapes_.contains(lod_key)) continue;
            try { static_cast<void>(retain(lod_shapes_, lod_key,
                decoder_.decode_lod(base->header, pointer, colour))); }
            catch (const std::exception&) { ++stats.rejected; }
        }
    }
    std::size_t vertices = 0, faces = 0, polygon = 0;
    const auto measure = [&](const assets::Shape& shape) {
        vertices = std::max(vertices, shape.vertices.size());
        for (const auto& frame : shape.frames) vertices = std::max(vertices, frame.vertices.size());
        auto ordered = shape.faces.size();
        for (const auto& batch : shape.face_batches) ordered += batch.faces.size();
        faces = std::max(faces, ordered);
        for (const auto& face : shape.faces) polygon = std::max(polygon, face.vertex_indices.size());
        for (const auto& batch : shape.face_batches)
            for (const auto& face : batch.faces) polygon = std::max(polygon, face.vertex_indices.size());
    };
    for (const auto& [key, shape] : shapes_) measure(shape.shape);
    for (const auto& [key, shape] : lod_shapes_) measure(shape.shape);
    model_scratch_.transformed_vertices.reserve(vertices);
    model_scratch_.projected.reserve(vertices);
    model_scratch_.ordered_faces.reserve(faces);
    model_scratch_.camera_polygon.reserve(polygon + 8U);
    model_scratch_.camera_clipping_scratch.reserve(polygon + 8U);
    model_scratch_.polygon.reserve(polygon + 8U);
    model_scratch_.raster_polygon.reserve(polygon + 8U);
    model_scratch_.clipping_scratch.reserve(polygon + 8U);
    return stats;
}

unsigned Frame3ds::preload_active_shapes(const simulation::GameSimulation& game) {
    const auto before = shape_cache_insertions_;
    const auto special = colour_symbol("ID_1_C");
    const auto red = colour_symbol("RED_C");
    const auto white = colour_symbol("WHITE_C");
    for (const auto handle : game.draw_order()) {
        if (!game.objects().is_active(handle)) continue;
        const auto& object = game.objects().at(handle);
        if (object.shape == 0U || (object.strategy_flags[3] & 0x08U) != 0U) continue;
        const auto flags = object.strategy_flags[0];
        const auto colour = static_cast<std::uint16_t>(
            (flags & 0x40U) != 0U ? 0U
            : (flags & 0x02U) != 0U && (flags & 0x20U) == 0U
                ? ((flags & 0x01U) != 0U ? red : white)
                : ((flags & 0x01U) != 0U ? special : object.colour_table));
        const auto* base = load_base_shape(object.shape, colour);
        if (!base) continue;
        for (auto pointer : {base->header.lod1_pointer, base->header.lod2_pointer,
                 base->header.lod3_pointer, base->header.shadow_pointer})
            static_cast<void>(load_lod_shape(*base, pointer, colour));
    }
    return static_cast<unsigned>(shape_cache_insertions_ - before);
}

bool Frame3ds::can_reuse_presentation(
    const simulation::GameSimulation& game, bool gpu_bg2,
    double interpolation_alpha) const {
    if (!last_draw_valid_ || last_frame_gpu_bg2_ != gpu_bg2
        || last_interpolation_alpha_ != interpolation_alpha
        || last_flow_ != game.flow_state()
        || last_scene_revision_ != game.scene_revision()) return false;
    const auto& map = game.map();
    const auto& ppu = map.ppu_state();
    if (gpu_bg2) {
        if (map.native_model_draw().active
            || last_ppu_memory_revision_ != map.ppu_memory_revision()
            || !same_mode2_foreground(last_ppu_, ppu)) return false;
        return true;
    }
    if (last_ppu_memory_revision_ != map.ppu_memory_revision()
        || !same_ppu_metadata(last_ppu_, ppu)) return false;
    const auto x = ppu.background_mode == 3U ? ppu.bg2_scroll_x
        : static_cast<std::int16_t>(map.read_native_word(ram_symbol("BG2XSCROLL")));
    const auto y = ppu.background_mode == 3U ? ppu.bg2_scroll_y
        : static_cast<std::int16_t>(map.read_native_word(ram_symbol("BG2SCROLL")));
    return x == background_scroll_x_ && y == background_scroll_y_;
}

std::uint32_t Frame3ds::ram_symbol(const char *name) const {
    for (const auto address : symbols_.find(name)) {
        if ((address >> 16U) == 0U || (address >> 16U) == 0x7eU)
            return address;
    }
    return 0U;
}

std::uint32_t Frame3ds::fx_symbol(const char *name) const {
    for (const auto address : symbols_.find(name)) {
        if ((address >> 16U) == 0x70U) return address;
    }
    return 0U;
}

std::uint16_t Frame3ds::colour_symbol(const char *name) const {
    for (const auto address : symbols_.find(name)) {
        if ((address & 0xffffU) >= 0x8000U
            && ((address >> 16U) & 0xffU) < 0x70U)
            return static_cast<std::uint16_t>(address);
    }
    return 0U;
}

std::pair<std::int16_t, std::int16_t>
Frame3ds::current_background_scroll(
    const simulation::GameSimulation& game, double interpolation_alpha) const {
    const auto& map = game.map();
    const auto& ppu = map.ppu_state();
    if (ppu.background_mode == 3U)
        return {ppu.bg2_scroll_x, ppu.bg2_scroll_y};
    const auto x = static_cast<std::int16_t>(map.peek_ram_word(ram_symbol("BG2XSCROLL")).value_or(0));
    const auto y = static_cast<std::int16_t>(map.peek_ram_word(ram_symbol("BG2SCROLL")).value_or(0));
    if (ppu.background_mode == 2U && !game.paused()
        && (game.flow_state() == simulation::GameFlowState::gameplay
            || game.flow_state() == simulation::GameFlowState::training)
        && bg2_motion_.matches_source(ppu, x, y, map.background()))
        return bg2_motion_.scroll(interpolation_alpha);
    return {x, y};
}

const simulation::SnesPpuState& Frame3ds::presentation_ppu(
    const simulation::GameSimulation& game, double interpolation_alpha) {
    const auto& map = game.map();
    const auto& ppu = map.ppu_state();
    if (interpolation_alpha >= 1.0 || game.paused() || ppu.background_mode != 2U
        || (game.flow_state() != simulation::GameFlowState::gameplay
            && game.flow_state() != simulation::GameFlowState::training)) return ppu;
    const auto x = static_cast<std::int16_t>(map.peek_ram_word(ram_symbol("BG2XSCROLL")).value_or(0));
    const auto y = static_cast<std::int16_t>(map.peek_ram_word(ram_symbol("BG2SCROLL")).value_or(0));
    if (!bg2_motion_.matches_source(ppu, x, y, map.background())) return ppu;
    presentation_ppu_ = ppu;
    bg2_motion_.apply(presentation_ppu_, interpolation_alpha);
    return presentation_ppu_;
}

Frame3ds::StorageStats Frame3ds::storage_stats() const noexcept {
    StorageStats result;
    result.bg2_presentation_fixed_bytes = sizeof(bg2_motion_) + sizeof(presentation_ppu_);
    for (const auto* frame : {&frame_, &wide_frame_, &world_, &right_world_,
            &right_frame_, &ex_overlay_, &mode2_background_cache_, &corner_hud_,
            &meter_hud_, &top_hud_cache_})
        result.framebuffer_capacity_bytes += frame->storage_capacity_bytes();
    for (const auto& cache : mode1_caches_)
        for (const auto* frame : {&cache.bg3_low, &cache.bg3_high,
                &cache.panorama_low, &cache.panorama_high,
                &cache.bg2_low, &cache.bg2_high, &cache.bg1})
            result.framebuffer_capacity_bytes += frame->storage_capacity_bytes();
    const auto bytes = [](const auto& values) {
        return values.capacity() * sizeof(values[0]);
    };
    result.item_capacity_bytes = bytes(items_) + bytes(world_cache_render_items_);
    result.draw_order_capacity_bytes = bytes(world_cache_draw_order_);
    result.object_capacity_bytes = bytes(world_cache_objects_);
    result.foreground_spans_size = source_foreground_spans_.size();
    result.foreground_spans_capacity = source_foreground_spans_.capacity();
    result.hud_spans_size = top_hud_spans_.size();
    result.hud_spans_capacity = top_hud_spans_.capacity();
    result.spans_capacity_bytes = bytes(source_foreground_spans_) + bytes(top_hud_spans_);
    result.scratch_capacity_bytes = bytes(model_scratch_.transformed_vertices)
        + bytes(model_scratch_.projected) + bytes(model_scratch_.ordered_faces)
        + bytes(model_scratch_.camera_polygon) + bytes(model_scratch_.camera_clipping_scratch)
        + bytes(model_scratch_.polygon) + bytes(model_scratch_.raster_polygon)
        + bytes(model_scratch_.clipping_scratch);
    result.interpolation_entries = previous_objects_.size() + current_objects_.size();
    result.interpolation_buckets = previous_objects_.bucket_count() + current_objects_.bucket_count();
    result.fixed_particle_dust_bytes = sizeof(world_cache_particles_) + sizeof(world_cache_dust_);
    result.tracked_vector_capacity_bytes = result.framebuffer_capacity_bytes
        + result.item_capacity_bytes + result.draw_order_capacity_bytes
        + result.object_capacity_bytes + result.spans_capacity_bytes
        + result.scratch_capacity_bytes;
    return result;
}

void Frame3ds::set_stereo_strength(std::uint8_t strength) noexcept {
    strength = std::min<std::uint8_t>(strength, 8U);
    if (strength != stereo_strength_) {
        stereo_strength_ = strength;
        last_draw_valid_ = false;
    }
}



const render::Framebuffer& Frame3ds::draw(
    const simulation::GameSimulation& game, bool gpu_bg2,
    double interpolation_alpha, const simulation::SnesPpuState* prepared_background) {
    const auto stage_begin = svcGetSystemTick();
    // No model pointer escapes a draw. Release oversized demand-decoded
    // fallbacks at this boundary instead of retaining them for the session.
    transient_base_shape_.reset();
    transient_lod_shape_.reset();
    world_cache_snapshot_us_ = 0U;
    if (!snapshots_ready_) capture_after_tick(game);
    interpolation_alpha = std::clamp(interpolation_alpha, 0.0, 1.0);
#ifdef STARWING_DEEP_PROFILE
    const auto profile_begin = osGetTime();
    static unsigned mode1_hits = 0U, mode1_builds = 0U;
    static unsigned mode2_hits = 0U, mode2_builds = 0U;
    static unsigned mode1_vram_changes = 0U, mode1_scroll_changes = 0U;
    static unsigned mode1_other_changes = 0U;
    static unsigned mode1_main_changes = 0U, mode1_mosaic_changes = 0U;
    static unsigned mode1_bg2_extra_changes = 0U, mode1_base_changes = 0U;
    static unsigned mode1_tunnel_changes = 0U;
    static unsigned mode1_bg1_builds = 0U, mode1_bg2_builds = 0U;
    static unsigned mode1_bg3_builds = 0U;
#endif
    const auto& map = game.map();
    const auto& ppu = prepared_background ? *prepared_background
        : presentation_ppu(game, interpolation_alpha);
    // Both priority passes of every layer in this frame read this one
    // immutable PPU snapshot; let them share decoded characters.
    backgrounds_.begin_frame(ppu);
    struct EndBackgroundFrame {
        render::BackgroundRenderer& renderer;
        ~EndBackgroundFrame() { renderer.end_frame(); }
    } end_background_frame{backgrounds_};
    const auto read_ram = [&](const char *name) {
        return map.read_native_word(ram_symbol(name));
    };
    const auto read_fx = [&](const char *name) {
        return map.read_native_word(fx_symbol(name));
    };
    const auto [background_x, background_y] =
        current_background_scroll(game, interpolation_alpha);
    background_scroll_x_ = background_x;
    background_scroll_y_ = background_y;

    const bool mode2_gameplay = ppu.background_mode == 2U
        && (game.flow_state() == simulation::GameFlowState::gameplay
            || game.flow_state() == simulation::GameFlowState::training);
    const bool level_flow = game.flow_state() == simulation::GameFlowState::gameplay
        || game.flow_state() == simulation::GameFlowState::training;
    const bool native_mode1_background = ppu.background_mode == 1U && level_flow;
    const bool stereo = mode2_gameplay && stereo_strength_ != 0U;
    const bool native_world = native_engine_enabled_ && ((gpu_bg2 && mode2_gameplay) || (native_mode1_enabled_ && native_mode1_background)) && !stereo;
    native_scene_active_=false;
    if (native_world != last_native_world_) world_cache_valid_=false;
    last_native_world_=native_world;
    const bool cache_source_foreground = mode2_gameplay
        && !map.native_model_draw().active && !game.briefing_state().active;
    // Original Mode 2 has only OBJ and pause text in the source foreground.
    // A BG2 table update or a changing status meter must not redraw it. EX
    // additionally draws BG1 with CGRAM-dependent black rejection, so retain
    // the broader memory token there.
    const bool precise_source_sprites = game.experience()
        == simulation::Experience::original;
    const bool reuse_source_foreground = cache_source_foreground
        && source_foreground_valid_
        && source_foreground_paused_ == game.paused()
        && source_foreground_experience_ == game.experience()
        && last_flow_ == game.flow_state()
        && last_scene_revision_ == game.scene_revision()
        && same_mode2_foreground(last_ppu_, ppu)
        && (last_ppu_memory_revision_ == map.ppu_memory_revision()
            || (precise_source_sprites
                && source_sprite_cache_.matches(ppu, map.ppu_vram_revision())));
    if (cache_source_foreground) {
        if (reuse_source_foreground) {
            ++source_foreground_cache_hits_;
            if (last_ppu_memory_revision_ != map.ppu_memory_revision())
                ++source_foreground_selective_hits_;
        } else ++source_foreground_cache_misses_;
    }
    // Keep the cartridge's status sprites available for both screens, but
    // compose them only after the source raster has reached native width.
    const auto draw_source_sprites = [&](std::uint8_t priority) {
        if (reuse_source_foreground) return;
        sprites_.draw_objects(ppu, frame_, priority, 0, true, false,
            nullptr, level_flow, nullptr, false, level_flow);
    };
    if (!reuse_source_foreground) frame_.clear(0U);
    // The PICA BG2 path needs exact foreground coverage: palette index zero
    // can be an opaque sprite or model pixel, not just an empty pixel.
    if (mode2_gameplay && !reuse_source_foreground) frame_.begin_write_coverage();
    Mode1BackgroundCache* mode1_cache = nullptr;
    const auto restore_mode1 = [this](const render::Framebuffer& layer) {
        render::LayerCompositeSettings placement{};
        if (layer.width() == 400U) {
            placement.offset_x = -72;
            placement.offset_y = -8;
        }
        render::composite_transparent_layer(layer, frame_, placement);
    };
    if (ppu.background_mode == 1U) {
        // Gameplay's Super FX BG1 is replaced by the native world raster.
        // Only the title composites this decoded layer. Track its validity
        // separately so a later title frame cannot reuse an omitted build.
        const bool use_mode1_bg1 = game.flow_state()
            == simulation::GameFlowState::title;
        // The title repeatedly switches between two character bases while
        // leaving VRAM untouched. Keep both decoded layer variants so that
        // each switch does not rasterize the entire SNES background again.
        for (auto& candidate : mode1_caches_) {
            if (candidate.valid
                && (candidate.bg2_low.width() == 400U) == native_mode1_background
                && candidate.ppu_vram_revision == map.ppu_vram_revision()
                && (!use_mode1_bg1 || (candidate.bg1_valid
                    && same_mode1_bg1(candidate.ppu, ppu)))
                && same_bg2_tiles(candidate.ppu, ppu)
                && candidate.scroll_x == background_x
                && candidate.scroll_y == background_y
                && same_mode1_bg3(candidate.ppu, ppu)) {
                mode1_cache = &candidate;
                break;
            }
        }
        if (mode1_cache == nullptr) {
            for (auto& candidate : mode1_caches_) {
                if (!candidate.valid) {
                    mode1_cache = &candidate;
                    break;
                }
            }
        }
        if (mode1_cache == nullptr) {
            mode1_cache = &mode1_caches_[mode1_next_victim_];
            mode1_next_victim_ = static_cast<std::uint8_t>(
                (mode1_next_victim_ + 1U) % mode1_caches_.size());
        }
        auto& cache = *mode1_cache;
        const auto width = native_mode1_background ? 400U : 256U;
        const auto height = native_mode1_background ? 240U : 224U;
        if (cache.bg2_low.width() != width || cache.bg2_low.height() != height) {
            cache.bg2_low.resize(width, height);
            cache.bg2_high.resize(width, height);
            cache.valid = false;
        }
        if (!native_mode1_background && cache.bg3_low.width() != 256U) {
            cache.bg3_low.resize(256U, 224U);
            cache.bg3_high.resize(256U, 224U);
            cache.valid = false;
        }
        const bool same_bg1 = !use_mode1_bg1 || (cache.valid
            && cache.bg1_valid
            && cache.ppu_vram_revision == map.ppu_vram_revision()
            && same_mode1_bg1(cache.ppu, ppu));
        const bool same_bg2 = cache.valid
            && cache.ppu_vram_revision == map.ppu_vram_revision()
            && same_bg2_tiles(cache.ppu, ppu)
            && cache.scroll_x == background_x
            && cache.scroll_y == background_y
            && (!ppu.tunnel_scene || cache.cgram == ppu.cgram);
        const bool same_bg3 = cache.valid
            && cache.ppu_vram_revision == map.ppu_vram_revision()
            && same_mode1_bg3(cache.ppu, ppu);
        const bool same_atlas = cache.valid
            && cache.ppu_vram_revision == map.ppu_vram_revision()
            && same_mode1_bg3_tiles(cache.ppu, ppu)
            && std::equal(ppu.cgram.begin(), ppu.cgram.begin() + 32U,
                cache.cgram.begin());
        const bool same_panorama = !native_mode1_background || (same_bg3
            && same_atlas && cache.panorama_valid);
        const bool same_background = same_bg1 && same_bg2 && same_bg3 && same_panorama;
        if (same_background) ++background_cache_hits_;
        else ++background_cache_misses_;
#ifdef STARWING_DEEP_PROFILE
        if (same_background) ++mode1_hits;
        else {
            ++mode1_builds;
            if (!same_bg1) ++mode1_bg1_builds;
            if (!same_bg2) ++mode1_bg2_builds;
            if (!same_bg3) ++mode1_bg3_builds;
            if (cache.valid) {
                if (cache.ppu.vram != ppu.vram) ++mode1_vram_changes;
                if (cache.scroll_x != background_x
                    || cache.scroll_y != background_y
                    || cache.ppu.bg1_scroll_x != ppu.bg1_scroll_x
                    || cache.ppu.bg1_scroll_y != ppu.bg1_scroll_y
                    || cache.ppu.bg2_scroll_x != ppu.bg2_scroll_x
                    || cache.ppu.bg2_scroll_y != ppu.bg2_scroll_y
                    || cache.ppu.bg3_scroll_x != ppu.bg3_scroll_x
                    || cache.ppu.bg3_scroll_y != ppu.bg3_scroll_y)
                    ++mode1_scroll_changes;
                if (cache.ppu.vram == ppu.vram
                    && cache.scroll_x == background_x
                    && cache.scroll_y == background_y
                    && !same_mode1_tiles(cache.ppu, ppu))
                    ++mode1_other_changes;
                if (cache.ppu.main_screen != ppu.main_screen)
                    ++mode1_main_changes;
                if (cache.ppu.mosaic != ppu.mosaic)
                    ++mode1_mosaic_changes;
                if (cache.ppu.bg2_horizontal_offsets_enabled
                        != ppu.bg2_horizontal_offsets_enabled
                    || cache.ppu.bg2_vertical_offsets_enabled
                        != ppu.bg2_vertical_offsets_enabled
                    || cache.ppu.bg2_scanline_scroll_enabled
                        != ppu.bg2_scanline_scroll_enabled)
                    ++mode1_bg2_extra_changes;
                if (cache.ppu.bg1_character_base != ppu.bg1_character_base
                    || cache.ppu.bg2_character_base != ppu.bg2_character_base
                    || cache.ppu.bg3_character_base != ppu.bg3_character_base
                    || cache.ppu.bg1_screen_base != ppu.bg1_screen_base
                    || cache.ppu.bg2_screen_base != ppu.bg2_screen_base
                    || cache.ppu.bg3_screen_base != ppu.bg3_screen_base)
                    ++mode1_base_changes;
                if (cache.ppu.tunnel_scene != ppu.tunnel_scene
                    || (ppu.tunnel_scene
                        && cache.ppu.cgram != ppu.cgram))
                    ++mode1_tunnel_changes;
            }
        }
#endif
        if (native_mode1_background && !same_atlas) {
            // Decode the complete, stationary tilemap once. Rebuilding from
            // a scrolled 256-pixel screenshot makes donor patches change at
            // every pan and visibly remesh the added background columns.
            auto atlas_ppu = std::make_unique<simulation::SnesPpuState>(ppu);
            atlas_ppu->bg3_scroll_x = 0;
            atlas_ppu->bg3_scroll_y = 0;
            atlas_ppu->mosaic = 0;
            const auto edge = ppu.bg3_tile_size_16 ? 16U : 8U;
            const auto atlas_width = ((ppu.bg3_screen_size & 1U) != 0U ? 64U : 32U) * edge;
            const auto atlas_height = ((ppu.bg3_screen_size & 2U) != 0U ? 64U : 32U) * edge;
            render::Framebuffer low(atlas_width, atlas_height), high(atlas_width, atlas_height);
            backgrounds_.draw_bg3_split(*atlas_ppu, low, high);
            build_native_panorama_atlas(low, high, cache.bg3_low,
                cache.bg3_high, ppu);
        } else if (!native_mode1_background && !same_bg3) {
            cache.bg3_low.clear();
            cache.bg3_high.clear();
            backgrounds_.draw_bg3_split(ppu,
                cache.bg3_low, cache.bg3_high);
        }
        if (native_mode1_background && !same_panorama) {
            scroll_native_panorama_atlas(cache.bg3_low, cache.bg3_high,
                cache.panorama_low, cache.panorama_high, ppu);
            cache.panorama_valid = true;
        }
        if (!same_bg2) {
            cache.bg2_low.clear();
            cache.bg2_high.clear();
            backgrounds_.draw_bg2_mode1_split(ppu, background_x,
                background_y, cache.bg2_low, cache.bg2_high,
                native_mode1_background ? 72 : 0,
                native_mode1_background ? 8 : 0);
        }
        if (!same_bg1) {
            cache.bg1.clear();
            backgrounds_.draw_bg1(ppu, cache.bg1, render::TilePriorityPass::all);
        }
        if (!same_background) {
            capture_ppu_metadata(cache.ppu, ppu);
            cache.cgram = ppu.cgram;
            cache.ppu_vram_revision = map.ppu_vram_revision();
            cache.scroll_x = background_x;
            cache.scroll_y = background_y;
            cache.valid = true;
            cache.bg1_valid = use_mode1_bg1;
        }
        restore_mode1(native_mode1_background ? cache.panorama_low : cache.bg3_low);
        draw_source_sprites(0U);
        if (!ppu.bg3_high_priority)
            restore_mode1(native_mode1_background ? cache.panorama_high : cache.bg3_high);
        draw_source_sprites(1U);
        restore_mode1(cache.bg2_low);
    } else if (mode2_gameplay) {
        // Keep only models and OBJ coverage in the source-sized foreground.
        // BG2 is owned by PICA or the native CPU fallback. Drawing it here
        // marks the whole 256x224 source as foreground and hides the world
        // geometry in the centre when that foreground is placed over BG2.
    } else if (ppu.background_mode == 2U) {
        backgrounds_.draw_bg2(ppu, background_x, background_y,
            frame_, render::TilePriorityPass::low);
        draw_source_sprites(0U);
        draw_source_sprites(1U);
        backgrounds_.draw_bg2(ppu, background_x, background_y,
            frame_, render::TilePriorityPass::high);
    } else if (ppu.background_mode == 3U) {
        backgrounds_.draw_bg2(ppu, background_x, background_y,
            frame_, render::TilePriorityPass::low);
        draw_source_sprites(0U);
        backgrounds_.draw_bg1(ppu, frame_, render::TilePriorityPass::low);
        draw_source_sprites(1U);
        backgrounds_.draw_bg2(ppu, background_x, background_y,
            frame_, render::TilePriorityPass::high);
        draw_source_sprites(2U);
        backgrounds_.draw_bg1(ppu, frame_, render::TilePriorityPass::high);
    } else {
        backgrounds_.draw_bg2(ppu, background_x, background_y, frame_);
        backgrounds_.draw_bg3(ppu, frame_);
    }
    std::array<std::uint8_t, 224U> background_left_edge{};
    std::array<std::uint8_t, 224U> background_right_edge{};
    const auto native_world_scene = game.flow_state()
            == simulation::GameFlowState::gameplay
        || game.flow_state() == simulation::GameFlowState::training
        || game.flow_state() == simulation::GameFlowState::intro;
    if (native_world_scene && !mode2_gameplay) {
        const auto& background = frame_.pixels();
        for (std::uint32_t y = 0U; y < 224U; ++y) {
            background_left_edge[y] = background[y * 256U];
            background_right_edge[y] = background[y * 256U + 255U];
        }
    }
    const auto stage_background = svcGetSystemTick();
#ifdef STARWING_DEEP_PROFILE
    const auto profile_background = osGetTime();
#endif

    const auto camera = timing::interpolate(
        previous_camera_, current_camera_, interpolation_alpha);
    const auto camera_x = static_cast<std::int16_t>(std::lround(camera.x));
    const auto camera_y = static_cast<std::int16_t>(std::lround(camera.y));
    const auto camera_z = static_cast<std::int16_t>(std::lround(camera.z));
    const auto camera_pitch = static_cast<std::int16_t>(
        std::lround(camera.pitch));
    const auto camera_yaw = static_cast<std::int16_t>(
        std::lround(camera.yaw));
    const auto camera_roll = static_cast<std::int16_t>(
        std::lround(camera.roll));
    const auto vanish_x = static_cast<std::int16_t>(read_fx("M_VANISHX"));
    const auto vanish_y = static_cast<std::int16_t>(read_fx("M_VANISHY"));
    const auto frame_number = static_cast<std::uint8_t>(
        map.read_native_byte(ram_symbol("GAMEFRAME")) & 0x7fU);
    const auto shadow_height = static_cast<std::int16_t>(
        read_ram("SHADOWHEIGHT"));
    const auto player_fly_mode = map.read_native_byte(
        ram_symbol("PLAYERFLYMODE"));
    const auto depth_colours = read_fx("M_DEPTHSTAB");
    const auto depth_thresholds = read_fx("M_DEPTHTABLE");
    const auto dots_mode = map.dots_mode();
    const auto dust_count = game.dust_point_count();
    const std::array<std::int32_t, 18U> world_key{
        camera_x, camera_y, camera_z, camera_pitch, camera_yaw, camera_roll,
        vanish_x, vanish_y, frame_number, shadow_height, player_fly_mode,
        static_cast<std::int32_t>(depth_colours),
        static_cast<std::int32_t>(depth_thresholds), dots_mode,
        static_cast<std::int32_t>(dust_count),
        static_cast<std::int32_t>(game.flow_state()),
        stereo ? stereo_strength_ : 0,
        interpolation_alpha > 0.0 && interpolation_alpha < 1.0};
    const auto& draw_order = game.draw_order();
    const auto world_cache_validation_begin = svcGetSystemTick();
    bool world_cache_hit = world_cache_valid_
        && world_cache_scene_revision_ == game.scene_revision()
        && world_cache_key_ == world_key
        && world_cache_draw_order_ == draw_order
        && world_cache_particles_ == game.particles().particles()
        && world_cache_dust_ == game.dust().points()
        && world_cache_objects_.size() == draw_order.size();
    if (world_cache_hit) {
        for (std::size_t index = 0; index < draw_order.size(); ++index) {
            const auto handle = draw_order[index];
            const auto active = game.objects().is_active(handle);
            if (active != world_cache_objects_[index].has_value()
                || (active && game.objects().at(handle)
                    != *world_cache_objects_[index])) {
                world_cache_hit = false;
                break;
            }
        }
    }
    const auto matrix_for = [this](const timing::TransformSnapshot& pose) {
        return simulation::rotation_matrix_q15(trigonometry_,
            static_cast<std::int16_t>(pose.pitch),
            static_cast<std::int16_t>(pose.yaw),
            static_cast<std::int16_t>(pose.roll));
    };
    const auto view = interpolation_alpha >= 1.0
        ? matrix_for(current_camera_)
        : simulation::interpolate_rotation_matrix_q15(
            matrix_for(previous_camera_), matrix_for(current_camera_),
            interpolation_alpha);
    const timing::RenderTransform raster_camera{
        static_cast<double>(camera_x), static_cast<double>(camera_y),
        static_cast<double>(camera_z), static_cast<double>(camera_pitch),
        static_cast<double>(camera_yaw), static_cast<double>(camera_roll)};
    const auto& flash_addresses = symbols_.find("FLASHPLAYER_STRAT");
    const auto flash_player_strategy = flash_addresses.empty()
        ? 0U : flash_addresses.front();
    items_.clear();
    items_.reserve(draw_order.size());
    for (const auto handle : draw_order) {
        if (!game.objects().is_active(handle)) continue;
        const auto& object = game.objects().at(handle);
        if ((object.strategy_flags[3] & 0x08U) != 0U || object.shape == 0U)
            continue;
        auto transform = timing::RenderTransform{
            static_cast<double>(object.world_x),
            static_cast<double>(object.world_y),
            static_cast<double>(object.world_z),
            static_cast<double>(static_cast<std::uint16_t>(object.rotation_x)
                << 8U),
            static_cast<double>(static_cast<std::uint16_t>(object.rotation_y)
                << 8U),
            static_cast<double>(static_cast<std::uint16_t>(object.rotation_z)
                << 8U)};
        auto object_matrix = simulation::transpose_q15(
            simulation::rotation_matrix_q15(trigonometry_,
                simulation::wrap16(-static_cast<std::int32_t>(
                    static_cast<std::uint16_t>(object.rotation_x) << 8U)),
                simulation::wrap16(-static_cast<std::int32_t>(
                    static_cast<std::uint16_t>(object.rotation_y) << 8U)),
                simulation::wrap16(-static_cast<std::int32_t>(
                    static_cast<std::uint16_t>(object.rotation_z) << 8U))));
        const auto now = current_objects_.find(handle);
        const auto before = previous_objects_.find(handle);
        if (now != current_objects_.end()) {
            const auto& current = now->second;
            const bool same_entity = before != previous_objects_.end()
                && before->second.generation == current.generation
                && before->second.shape == current.shape
                && before->second.type == current.type
                && before->second.strategy_address == current.strategy_address;
            auto render_previous = same_entity ? before->second : current;
            auto render_current = current;
            if (flash_player_strategy != 0U
                && object.strategy_address == flash_player_strategy) {
                // This alternating NULLSHAPE/wireframe is attached to the
                // Arwing. A shape change must not hold the effect at the next
                // source pose while the player's geometry is interpolating.
                // Preserve its source blink/material state; borrow only the
                // owner's position and orientation history.
                render::anchor_player_overlay(render_previous, render_current,
                    previous_objects_, current_objects_, game.player());
            }
            transform = timing::interpolate(render_previous.transform,
                render_current.transform, interpolation_alpha);
            object_matrix = render::interpolate_object_rotation(
                render_previous, render_current, interpolation_alpha, 0U);
        }
        const auto world_x = static_cast<std::int16_t>(std::lround(transform.x));
        const auto world_y = static_cast<std::int16_t>(std::lround(transform.y));
        const auto world_z = static_cast<std::int16_t>(std::lround(transform.z));
        const auto position = simulation::transform_q15(view, {
            simulation::subtract16(
                world_x, camera_x),
            simulation::subtract16(
                world_y, camera_y),
            simulation::subtract16(
                world_z, camera_z)});
        items_.push_back({handle, position, object_matrix, world_x, world_z});
    }
    // Alpha can advance while integer model positions and Q15 rotations stay
    // unchanged. Reuse only when the complete inputs consumed by the world
    // rasterizer are identical, including interpolation-derived poses.
    world_cache_hit = world_cache_hit
        && world_cache_view_ == view
        && world_cache_render_items_ == items_;
    world_cache_validation_us_ = static_cast<std::uint64_t>(
        (svcGetSystemTick() - world_cache_validation_begin)
            * (1000000.0 / SYSCLOCK_ARM11));
    const auto stage_world_prepare_end = svcGetSystemTick();
#ifdef STARWING_DEEP_PROFILE
    auto profile_items = osGetTime();
    auto profile_models = profile_items;
#endif
    if (world_cache_hit) {
        ++world_cache_hits_;
    } else {
        ++world_cache_misses_;
        const auto depth_symbols = symbols_.find("DEPTHTABLES");
        const auto depth_address = depth_symbols.empty() ? 0U : depth_symbols.front();
        const auto special_colour = colour_symbol("ID_1_C");
        const auto red_colour = colour_symbol("RED_C");
        const auto white_colour = colour_symbol("WHITE_C");
        const auto effective_colour = [&](const auto& object) {
            const auto flags = object.strategy_flags[0];
            if ((flags & 0x40U) != 0U) return std::uint16_t{};
            if ((flags & 0x02U) != 0U && (flags & 0x20U) == 0U)
                return static_cast<std::uint16_t>(
                    (flags & 0x01U) != 0U ? red_colour : white_colour);
            return static_cast<std::uint16_t>(
                (flags & 0x01U) != 0U ? special_colour : object.colour_table);
        };

        for (unsigned side = 0U; side < (stereo ? 2U : 1U); ++side) {
        auto& eye_world = side == 0U ? world_ : right_world_;
        eye_world.clear(0U);
        if (native_world) { native_commands_.reset(400,240); eye_world.record_to(&native_commands_); }
        const int eye = stereo ? (side == 0U ? -int(stereo_strength_)
            : int(stereo_strength_)) : 0;
        if (dots_mode < 0) {
            dust_.draw(game.dust(), dust_count, raster_camera, view, eye_world,
                0, 0, 0, 0, eye);
            if (game.flow_state() == simulation::GameFlowState::intro) {
                // Repeat only the camera-projected dust into the two native side
                // columns. All three projections share the same live 3D points,
                // so the margins turn and scroll with the cartridge's centre.
                dust_.draw(game.dust(), dust_count, raster_camera, view, eye_world,
                    -192, 0, 72, 400);
                dust_.draw(game.dust(), dust_count, raster_camera, view, eye_world,
                    192, 0, 0, 328);
            }
        } else if (dots_mode > 0)
            dust_.draw_grid(raster_camera, view, eye_world, eye);

        const auto display_frame = [frame_number](std::uint8_t object_frame) {
            return (object_frame & 0x80U) != 0U
                ? static_cast<std::uint32_t>(object_frame & 0x7fU)
                : static_cast<std::uint32_t>(frame_number);
        };
#ifdef STARWING_DEEP_PROFILE
        profile_items = osGetTime();
        profile_models = profile_items;
#endif

        const auto make_pose = [&](const RenderItem& item, bool shadow) {
            const auto& object = game.objects().at(item.handle);
            const bool true_colour_shadow =
                (object.strategy_flags[0] & 0x04U) != 0U;
            auto position = item.position;
            if (shadow && !true_colour_shadow) {
                position = simulation::transform_q15(view, {
                    simulation::subtract16(item.world_x, camera_x),
                    simulation::subtract16(shadow_height, camera_y),
                    simulation::subtract16(item.world_z, camera_z)});
            }
            render::RenderPose pose;
            pose.x = position[0];
            pose.y = position[1];
            pose.z = position[2];
            pose.pitch = static_cast<std::uint16_t>(object.rotation_x) << 8U;
            pose.yaw = static_cast<std::uint16_t>(object.rotation_y) << 8U;
            pose.roll = static_cast<std::uint16_t>(object.rotation_z) << 8U;
            // The native top-screen canvas centres the 256x224 cartridge raster
            // at (72,8), with the source Super FX origin another (16,16) in.
            pose.vanish_x = vanish_x + 88;
            pose.vanish_y = vanish_y + 24;
            auto object_matrix = item.rotation_matrix;
            if (shadow) {
                object_matrix[1] = object_matrix[4] = object_matrix[7] = 0;
                if (!true_colour_shadow) {
                    pose.force_colour = true;
                    pose.forced_colour = 0x09U;
                }
            }
            pose.rotation_matrix = compose_model_view_matrix(
                object_matrix, view, interpolation_alpha);
            pose.use_rotation_matrix = true;
            pose.animation_frame = display_frame(object.animation_frame);
            pose.colour_frame = display_frame(object.colour_frame);
            pose.explosion_progress = (object.flags & 0x01U) != 0U
                ? object.count : 0U;
            pose.texture_scroll_x = object.texture_scroll_x;
            pose.texture_scroll_y = object.texture_scroll_y;
            if (depth_address != 0U)
                render::apply_source_depth_tables(rom_, depth_address,
                    depth_thresholds, depth_colours, object.extended[21], pose);
            apply_stereo_eye(pose, eye);
            return pose;
        };

        if ((player_fly_mode & 0x08U) != 0U) {
            for (const auto& item : items_) {
                const auto& object = game.objects().at(item.handle);
                if ((object.strategy_flags[0] & 0x0cU) == 0U) continue;
                const auto colour = effective_colour(object);
                const auto* base = load_base_shape(object.shape, colour);
                if (base == nullptr) continue;
                const auto pointer = base->header.shadow_pointer;
                const auto* shape = load_lod_shape(*base, pointer, colour);
                if (!shape) continue;
                models_.draw(*shape, make_pose(item, true), eye_world,
                    false, nullptr, nullptr, nullptr, &model_scratch_);
            }
        }

        for (const auto& item : items_) {
            const auto& object = game.objects().at(item.handle);
            if ((object.strategy_flags[0] & 0x04U) != 0U) continue;
            const auto colour = effective_colour(object);
            // Precaching is optional. Intro, GAME OVER and later spawns can
            // reach this path with a cold cache and must still draw.
            const auto* base = load_base_shape(object.shape, colour);
            if (base == nullptr) continue;
            auto pose = make_pose(item, false);
            if ((object.strategy_flags[0] & 0x10U) != 0U) {
                particles_.draw_owner(game.particles(), item.handle,
                    pose, 1.0, eye_world);
                continue;
            }
            if ((object.strategy_flags[0] & 0x40U) != 0U) {
                text_.draw(object.colour_table, object.extended[21],
                    std::bit_cast<std::int8_t>(object.texture_scroll_x),
                    pose, eye_world);
                continue;
            }
            const auto pointer = assets::ShapeDecoder::select_lod_pointer(
                base->header, static_cast<double>(item.position[2]));
            const auto* shape = load_lod_shape(*base, pointer, colour);
            if (!shape) continue;
            if ((object.strategy_flags[0] & 0x20U) != 0U) {
                auto adjustment = static_cast<std::int16_t>(
                    std::bit_cast<std::int8_t>(object.texture_scroll_x));
                for (std::uint8_t shift = 0U;
                     shift < base->header.shift; ++shift)
                    adjustment = simulation::add16(adjustment, adjustment);
                auto diameter = simulation::add16(
                    base->header.size, adjustment);
                diameter = simulation::add16(diameter, diameter);
                pose.simple_scaled_sprite = true;
                pose.simple_sprite_colour = object.extended[21];
                pose.simple_sprite_world_size = diameter == 0 ? 1 : diameter;
            }
            models_.draw(*shape, pose, eye_world,
                false, nullptr, nullptr, nullptr, &model_scratch_);
        }
        eye_world.record_to(nullptr);
        } // Both eyes share decoded shapes, poses and source simulation.
        if (native_world && !native_scene_.compile(native_commands_)) {
            render::replay_raster_commands(native_commands_,world_,nullptr);
        }
#ifdef STARWING_DEEP_PROFILE
        profile_models = osGetTime();
#endif
        const auto world_cache_snapshot_begin = svcGetSystemTick();
        world_cache_key_ = world_key;
        world_cache_scene_revision_ = game.scene_revision();
        world_cache_draw_order_ = draw_order;
        world_cache_view_ = view;
        world_cache_render_items_ = items_;
        world_cache_objects_.clear();
        world_cache_objects_.reserve(draw_order.size());
        for (const auto handle : draw_order) {
            if (game.objects().is_active(handle))
                world_cache_objects_.emplace_back(game.objects().at(handle));
            else world_cache_objects_.emplace_back(std::nullopt);
        }
        world_cache_particles_ = game.particles().particles();
        world_cache_dust_ = game.dust().points();
        world_cache_valid_ = true;
        world_cache_snapshot_us_ = static_cast<std::uint64_t>(
            (svcGetSystemTick() - world_cache_snapshot_begin)
                * (1000000.0 / SYSCLOCK_ARM11));
    }

    native_scene_active_=native_world && !native_scene_.stats().overflow;
    const auto stage_world = svcGetSystemTick();
    // The wider world raster already uses full PPU coordinates. Preserve
    // projected geometry that crosses the original Super FX viewport edge.
    render::LayerCompositeSettings world_to_source{};
    world_to_source.offset_x = -72;
    world_to_source.offset_y = -8;
    if (native_scene_active_ && native_mode1_background) {
        if (native_background_.width()!=400U) {
            native_background_=render::Framebuffer(400,240);
            empty_native_=render::Framebuffer(400,240);
        }
        compose_native_mode1(frame_,empty_native_,mode1_cache->bg2_low,empty_native_,
            mode1_cache->panorama_low,ppu.bg3_high_priority?empty_native_:mode1_cache->panorama_high,
            native_background_,false);
        frame_.begin_write_coverage();
    } else if (!mode2_gameplay)
        render::composite_transparent_layer(world_, frame_, world_to_source);
    const auto stage_world_composite_end = svcGetSystemTick();

    const auto stage_native_model_begin = svcGetSystemTick();
    const auto& native_model = map.native_model_draw();
    if (native_model.active && native_model.shape != 0U) {
            if (const auto* shape = load_base_shape(native_model.shape, 0U)) {
                render::RenderPose pose;
                pose.x = native_model.x;
                pose.y = native_model.y;
                pose.z = native_model.z;
                pose.pitch = static_cast<std::uint16_t>(
                    (native_model.rotation_x & 0xffU) << 8U);
                pose.yaw = static_cast<std::uint16_t>(
                    (native_model.rotation_y & 0xffU) << 8U);
                pose.roll = static_cast<std::uint16_t>(
                    (native_model.rotation_z & 0xffU) << 8U);
                pose.rotation_matrix = simulation::rotation_matrix_q15(
                    trigonometry_, static_cast<std::int16_t>(pose.pitch),
                    static_cast<std::int16_t>(pose.yaw),
                    static_cast<std::int16_t>(pose.roll));
                pose.use_rotation_matrix = true;
                pose.vanish_x = native_model.vanish_x + 16;
                pose.vanish_y = native_model.vanish_y + 16;
                pose.animation_frame = native_model.animation_frame;
                pose.colour_frame = native_model.colour_frame;
                models_.draw(*shape, pose, frame_,
                    false, nullptr, nullptr, nullptr, &model_scratch_);
            }
    }
#ifdef STARWING_DEEP_PROFILE
    const auto profile_native = osGetTime();
#endif
    const auto stage_native_model_end = svcGetSystemTick();

    if (!reuse_source_foreground
        && game.experience() == simulation::Experience::starfox_ex) {
        ex_overlay_.clear(0U);
        backgrounds_.draw_bg1(ppu, ex_overlay_,
            render::TilePriorityPass::all, 0, false, 16U);
        for (std::uint32_t y = 0U; y < ex_overlay_.height(); ++y) {
            for (std::uint32_t x = 0U; x < ex_overlay_.width(); ++x) {
                const auto pixel = ex_overlay_.get(x, y);
                if (pixel != 0U && (ppu.cgram[pixel] & 0x7fffU) != 0U)
                    frame_.set(static_cast<std::int32_t>(x),
                        static_cast<std::int32_t>(y), pixel);
            }
        }
    }

    const auto briefing = game.briefing_state();
    if (briefing.active && briefing.message_address != 0U) {
        constexpr auto base = render::briefing_text_palette_base;
        text_.draw_game_text(briefing.message_address, 28, 171,
            frame_, base, 13U, 216,
            briefing.visible_message_characters);
    }

    if (ppu.background_mode == 1U) {
        draw_source_sprites(2U);
        if (game.flow_state() == simulation::GameFlowState::title) {
            // PUSH START belongs to the cartridge's high-priority BG2 tiles.
            // Leave the world and Arwing beneath them untouched; the prompt is
            // rendered on the lower 3DS screen instead.
            for (std::uint32_t y = 94U; y < 104U; ++y) {
                for (std::uint32_t x = 90U; x < 166U; ++x)
                    mode1_cache->bg2_high.set(x, y, 0U);
            }
            render::composite_transparent_layer(mode1_cache->bg2_high, frame_, {});
            render::composite_transparent_layer(mode1_cache->bg1, frame_, {});
            render::composite_transparent_layer(mode1_cache->bg3_high, frame_, {});
        } else {
            restore_mode1(mode1_cache->bg2_high);
        }
    } else if (ppu.background_mode == 2U) {
        if (mode2_gameplay) {
            draw_source_sprites(0U);
            draw_source_sprites(1U);
        }
        draw_source_sprites(2U);
    }
    if (!level_flow && show_top_hud_ && game.meter_state().enabled)
        sprites_.draw_meters(game.meter_state(), frame_);
    draw_source_sprites(3U);
    if (ppu.background_mode == 1U && ppu.bg3_high_priority)
        restore_mode1(native_mode1_background ? mode1_cache->panorama_high : mode1_cache->bg3_high);

    const auto dialogue = game.dialogue_state();
    if (!level_flow && show_top_hud_ && dialogue.active) {
        // The cartridge Super FX communications canvas is centred at
        // (16,16) inside the 256x224 top-screen raster.
        text_.draw_face(dialogue.portrait_frame, 64, 168, frame_,
            7U * 16U, dialogue.alternate_portraits);
        if (dialogue.text_visible) {
            auto text_y = dialogue.three_lines ? 153 : 169;
            const auto translated_lines =
                text_.translated_game_text_lines(dialogue.text_address, 92)
                    .size();
            if (translated_lines != 0U)
                text_y = std::min(text_y,
                    183 - 10 * (static_cast<int>(translated_lines) - 1));
            text_.draw_game_text(dialogue.text_address, 99, text_y + 17,
                frame_, 7U * 16U, 9U, 191);
            text_.draw_game_text(dialogue.text_address, 98, text_y + 16,
                frame_, 7U * 16U, std::nullopt, 190);
        }
        if (dialogue.meter_visible) {
            for (int y = 0; y < 12; ++y) {
                for (int x = 0; x < 44; ++x) {
                    if (x == 0 || x == 43 || y == 0 || y == 11)
                        frame_.set(98 + x, 193 + y, 126U);
                    else if (x >= 2 && x < 2 + dialogue.meter_health
                        && y >= 2 && y < 10)
                        frame_.set(98 + x, 193 + y, 114U);
                }
            }
        }
    }
    if (game.paused() && !reuse_source_foreground) {
        for (const auto address : symbols_.find("PAUSETXT")) {
            if ((address & 0xffffU) >= 0x8000U
                && ((address >> 16U) & 0xffU) < 0x70U) {
                text_.draw_game_text(address, 106, 106, frame_);
                break;
            }
        }
    }
#ifdef STARWING_DEEP_PROFILE
    static unsigned frame_count = 0U;
    static std::uint64_t background_ms = 0U, item_ms = 0U;
    static std::uint64_t model_ms = 0U, native_ms = 0U, finish_ms = 0U;
    ++frame_count;
    background_ms += profile_background - profile_begin;
    item_ms += profile_items - profile_background;
    model_ms += profile_models - profile_items;
    native_ms += profile_native - profile_models;
    finish_ms += osGetTime() - profile_native;
    if (frame_count >= 20U) {
        if (FILE *log = std::fopen("sdmc:/3ds/Starwing/perf_draw.log", "a")) {
            std::fprintf(log, "frames=%u background=%llu items=%llu models=%llu native=%llu finish=%llu mode1_hits=%u mode1_builds=%u mode1_vram=%u mode1_scroll=%u mode1_other=%u main=%u mosaic=%u bg2extra=%u base=%u tunnel=%u bg1build=%u bg2build=%u bg3build=%u mode2_hits=%u mode2_builds=%u\n",
                frame_count,
                static_cast<unsigned long long>(background_ms),
                static_cast<unsigned long long>(item_ms),
                static_cast<unsigned long long>(model_ms),
                static_cast<unsigned long long>(native_ms),
                static_cast<unsigned long long>(finish_ms),
                mode1_hits, mode1_builds, mode1_vram_changes,
                mode1_scroll_changes, mode1_other_changes,
                mode1_main_changes, mode1_mosaic_changes,
                mode1_bg2_extra_changes, mode1_base_changes,
                mode1_tunnel_changes,
                mode1_bg1_builds, mode1_bg2_builds, mode1_bg3_builds,
                mode2_hits, mode2_builds);
            std::fclose(log);
        }
        frame_count = 0U;
        background_ms = item_ms = model_ms = native_ms = finish_ms = 0U;
        mode1_hits = mode1_builds = mode2_hits = mode2_builds = 0U;
        mode1_vram_changes = mode1_scroll_changes = mode1_other_changes = 0U;
        mode1_main_changes = mode1_mosaic_changes = 0U;
        mode1_bg2_extra_changes = mode1_base_changes = mode1_tunnel_changes = 0U;
        mode1_bg1_builds = mode1_bg2_builds = mode1_bg3_builds = 0U;
    }
#endif
    if (mode2_gameplay) {
        frame_.end_write_coverage();
        if (!reuse_source_foreground)
            capture_native_coverage_spans(frame_, source_foreground_spans_);
        if (!gpu_bg2) wide_frame_.clear(0U);
        if (stereo) {
            right_frame_.end_write_coverage();
            if (!gpu_bg2) right_frame_.clear(0U);
        }
        if (!gpu_bg2) {
            const bool same_background = mode2_background_valid_
                && mode2_background_scene_revision_ == game.scene_revision()
                && mode2_background_ppu_vram_revision_
                    == map.ppu_vram_revision()
                && mode2_background_cgram_ == ppu.cgram
                && mode2_background_x_ == background_x
                && mode2_background_y_ == background_y
                && same_bg2_tiles(mode2_background_ppu_, ppu);
#ifdef STARWING_DEEP_PROFILE
            if (same_background) ++mode2_hits;
            else ++mode2_builds;
#endif
            if (same_background) ++background_cache_hits_;
            else ++background_cache_misses_;
            if (same_background) {
                wide_frame_.copy_pixels_from(mode2_background_cache_);
            } else {
                backgrounds_.draw_bg2(ppu, background_x, background_y,
                    wide_frame_, render::TilePriorityPass::all,
                    72, true, true, false, 0U, {}, 8);
                mode2_background_cache_.copy_pixels_from(wide_frame_);
                capture_ppu_metadata(mode2_background_ppu_, ppu);
                mode2_background_cgram_ = ppu.cgram;
                mode2_background_ppu_vram_revision_
                    = map.ppu_vram_revision();
                mode2_background_scene_revision_ = game.scene_revision();
                mode2_background_x_ = background_x;
                mode2_background_y_ = background_y;
                mode2_background_valid_ = true;
            }
        }
        if (stereo && !gpu_bg2) {
            stereo_background(mode2_background_cache_, wide_frame_, -int(stereo_strength_));
            stereo_background(mode2_background_cache_, right_frame_, int(stereo_strength_));
        }
        if (native_scene_active_) { wide_frame_.clear(); wide_frame_.begin_write_coverage(); }
        else if (gpu_bg2) wide_frame_.assign_transparent_layer(world_, true);
        else render::composite_transparent_layer(world_, wide_frame_, {});
        overlay_source_coverage_on_native(frame_, wide_frame_, source_foreground_spans_);
        if (stereo) {
            if (gpu_bg2) right_frame_.assign_transparent_layer(right_world_, true);
            else render::composite_transparent_layer(right_world_, right_frame_, {});
            overlay_source_coverage_on_native(frame_, right_frame_, source_foreground_spans_);
        }
    } else if (native_mode1_background && native_scene_active_) {
        wide_frame_.clear(); wide_frame_.begin_write_coverage();
        render::composite_transparent_layer(mode1_cache->bg2_high,wide_frame_,{});
        if (ppu.bg3_high_priority)
            render::composite_transparent_layer(mode1_cache->panorama_high,wide_frame_,{});
        frame_.end_write_coverage();
        capture_native_coverage_spans(frame_,source_foreground_spans_);
        overlay_source_coverage_on_native(frame_,wide_frame_,source_foreground_spans_);
    } else if (native_mode1_background) {
        // All background pixels share one scroll coordinate system. The
        // source-sized foreground keeps OBJ, models and their SNES priority;
        // copying its centre no longer reinstates wrapped scenery seams.
        compose_native_mode1(frame_, world_, mode1_cache->bg2_low,
            mode1_cache->bg2_high, mode1_cache->panorama_low,
            mode1_cache->panorama_high, wide_frame_, ppu.bg3_high_priority);
    } else {
        const auto flow = game.flow_state();
        const auto style = flow == simulation::GameFlowState::gameplay
                || flow == simulation::GameFlowState::training
            ? NativeCanvasStyle::gameplay
            : flow == simulation::GameFlowState::intro
            ? NativeCanvasStyle::intro
            : flow == simulation::GameFlowState::controls_type
                || flow == simulation::GameFlowState::controls_choice
            ? NativeCanvasStyle::controls
            : flow == simulation::GameFlowState::planet_select
            ? NativeCanvasStyle::star_map
            : flow == simulation::GameFlowState::planet_travel
                || flow == simulation::GameFlowState::title
            ? NativeCanvasStyle::space
            : flow == simulation::GameFlowState::game_over
            ? NativeCanvasStyle::game_over
            : NativeCanvasStyle::plain;
        const int intro_vertical_shift=style==NativeCanvasStyle::intro
            ? static_cast<int>(std::lround(std::clamp(256.0*std::tan(
                static_cast<double>(camera_pitch)*
                (6.283185307179586/65536.0)), -4096.0, 4096.0))) : 0;
        compose_native_canvas(frame_, world_, wide_frame_, ppu, style,
            native_world_scene ? std::span<const std::uint8_t>{
                background_left_edge} : std::span<const std::uint8_t>{},
            native_world_scene ? std::span<const std::uint8_t>{
                background_right_edge} : std::span<const std::uint8_t>{},
            intro_vertical_shift);
    }
    if (level_flow && show_top_hud_) {
        const auto meters = game.meter_state();
        const bool hud_cache_hit=top_hud_cache_valid_
            && top_hud_object_select_==ppu.object_select
            && top_hud_meters_==meters
            && top_hud_experience_==game.experience()
            && top_hud_objects_enabled_==((ppu.main_screen & 0x10U)!=0U)
            && (top_hud_ppu_memory_revision_==map.ppu_memory_revision()
                || hud_sprite_cache_.matches(ppu,map.ppu_vram_revision()));
        if(hud_cache_hit) {
            ++top_hud_cache_hits_;
            if(top_hud_ppu_memory_revision_!=map.ppu_memory_revision())
                ++top_hud_selective_hits_;
        } else ++top_hud_cache_misses_;
        if(!hud_cache_hit) {
        top_hud_cache_.clear(0U);
        // All groups consume the same synthetic meter layer. Rasterize it
        // once, then crop it independently beside each source OBJ group.
        meter_hud_.clear(0U);
        if(meters.enabled) sprites_.draw_meters(meters,meter_hud_);
        // Source OAM labels/icons and synthetic bars have opposite vertical
        // order in the cartridge. Keep them as independent layers so the
        // scaled top HUD always shows the label/icons ABOVE the bar.
        const auto bounds=[&](const render::Framebuffer& source,
            int x0,int y0,int w,int h) {
            int left=x0+w,top=y0+h,right=x0-1,bottom=y0-1;
            for(int y=y0;y<y0+h;++y) for(int x=x0;x<x0+w;++x)
                if(source.get(x,y)!=0U) {
                    left=std::min(left,x);right=std::max(right,x);
                    top=std::min(top,y);bottom=std::max(bottom,y);
                }
            return HudRect{left,top,right-left+1,bottom-top+1};
        };
        const auto scaled=[&](int n) { return wide_view_ ? (n*3+1)/2 : n; };
        const int hud_left=wide_view_?8:80, hud_right=wide_view_?392:320;
        const int hud_top=wide_view_?8:16, hud_bottom=wide_view_?232:224;
        const auto blit=[&](const render::Framebuffer& source,
            HudRect crop,int x,int y) {
            for(int dy=0;dy<scaled(crop.h);++dy) {
                auto row=top_hud_cache_.raster_row(y+dy);
                for(int dx=0;dx<scaled(crop.w);++dx) {
                    const auto pixel=source.get(
                        crop.x+(wide_view_?dx*2/3:dx),crop.y+(wide_view_?dy*2/3:dy));
                    if(pixel!=0U) row.set(x+dx,pixel);
                }
            }
        };
        const auto draw_sprites=[&](render::HudElement element) {
            corner_hud_.clear(0U);
            sprites_.draw_objects(ppu,corner_hud_,std::nullopt,0,false,
                false,nullptr,false,&meters,true,true,element);
        };
        const auto blit_corner=[&](render::HudElement element,int x,int y,
            int w,int h,bool right) {
            draw_sprites(element);
            const auto crop=bounds(corner_hud_,x,y,w,h);
            if(crop.w<=0||crop.h<=0) return;
            blit(corner_hud_,crop,right?hud_right-scaled(crop.w):hud_left,hud_top);
        };
        const auto blit_stack=[&](render::HudElement element,int x,
            bool right) {
            draw_sprites(element);
            const auto upper=bounds(corner_hud_,x,160,112,64);
            const auto bar=bounds(meter_hud_,x,160,112,64);
            if(upper.w<=0||upper.h<=0) {
                if(bar.w>0&&bar.h>0)
                    blit(meter_hud_,bar,right?hud_right-scaled(bar.w):hud_left,
                        hud_bottom-scaled(bar.h));
                return;
            }
            if(bar.w<=0||bar.h<=0) {
                blit(corner_hud_,upper,right?hud_right-scaled(upper.w):hud_left,
                    hud_bottom-scaled(upper.h));
                return;
            }
            auto layout=stack_hud(scaled(upper.w),scaled(upper.h),
                scaled(bar.w),scaled(bar.h),right,wide_view_?400:256,wide_view_?240:224);
            if (!wide_view_) {
                layout.upper.x+=72; layout.bar.x+=72;
                layout.upper.y+=8; layout.bar.y+=8;
            }
            blit(corner_hud_,upper,layout.upper.x,layout.upper.y);
            blit(meter_hud_,bar,layout.bar.x,layout.bar.y);
        };
        const bool ex=game.experience()==simulation::Experience::starfox_ex;
        blit_corner(render::HudElement::lives,0,ex?160:0,112,
            ex?64:40,false);
        if(meters.boss_max_health!=0U) {
            draw_sprites(render::HudElement::boss_health);
            for(int y=0;y<40;++y) for(int x=96;x<256;++x) {
                const auto pixel=meter_hud_.get(x,y);
                if(pixel!=0U) corner_hud_.set(x,y,pixel);
            }
            const auto crop=bounds(corner_hud_,96,0,160,40);
            if(crop.w>0&&crop.h>0)
                blit(corner_hud_,crop,hud_right-scaled(crop.w),hud_top);
        }
        blit_stack(render::HudElement::shield,0,false);
        blit_stack(render::HudElement::bombs_boost,144,true);
        hud_sprite_cache_.capture(ppu,map.ppu_vram_revision());
        top_hud_object_select_=ppu.object_select;
        top_hud_objects_enabled_=(ppu.main_screen & 0x10U)!=0U;
        top_hud_meters_=meters;
        top_hud_experience_=game.experience();
        top_hud_cache_valid_=true;
        top_hud_spans_.clear();
        const auto& cached_pixels=top_hud_cache_.pixels();
        for(std::uint32_t index=0;index<cached_pixels.size();) {
            while(index<cached_pixels.size()&&cached_pixels[index]==0U) ++index;
            const auto first=index;
            while(index<cached_pixels.size()&&cached_pixels[index]!=0U) ++index;
            if(index!=first) top_hud_spans_.push_back({first,first,index-first});
        }
        }
        // An exact match also validates this epoch. Subsequent interpolated
        // frames can then take the constant-time memory-token fast path.
        top_hud_ppu_memory_revision_=map.ppu_memory_revision();
        // Sprite artwork changes at source cadence, while the world can
        // interpolate on every presentation. Reuse opaque positions while
        // the cached HUD artwork is unchanged.
        const auto& pixels=top_hud_cache_.pixels();
        const auto overlay_hud = [&](render::Framebuffer& target) {
            auto& destination = target.pixels();
            auto& tags = target.layer_tags();
            const auto tag = target.layer_override() < 0
                ? static_cast<std::uint8_t>(render::PixelLayer::two_d)
                : static_cast<std::uint8_t>(target.layer_override());
            for(const auto& span:top_hud_spans_) {
                std::copy_n(pixels.data()+span.source_offset,span.count,
                    destination.data()+span.target_offset);
                target.mark_written(span.target_offset,span.count);
                if(target.layer_tags_enabled())
                    std::fill_n(tags.data()+span.target_offset,span.count,tag);
            }
        };
        overlay_hud(wide_frame_);
        if (stereo) overlay_hud(right_frame_);
    }
    if ((mode2_gameplay && gpu_bg2) || native_scene_active_) wide_frame_.end_write_coverage();
    if (stereo && gpu_bg2) right_frame_.end_write_coverage();
    last_draw_stereo_ = stereo;
    if (cache_source_foreground && !reuse_source_foreground && precise_source_sprites)
        source_sprite_cache_.capture(ppu, map.ppu_vram_revision());
    source_foreground_valid_ = cache_source_foreground;
    source_foreground_paused_ = game.paused();
    source_foreground_experience_ = game.experience();
    capture_ppu_metadata(last_ppu_, ppu);
    last_ppu_memory_revision_ = game.map().ppu_memory_revision();
    last_flow_ = game.flow_state();
    last_scene_revision_ = game.scene_revision();
    last_frame_gpu_bg2_ = mode2_gameplay && gpu_bg2;
    last_frame_wide_ = true;
    last_interpolation_alpha_ = interpolation_alpha;
    last_draw_valid_ = true;
    const auto stage_finish = svcGetSystemTick();
    constexpr auto tick_to_ms = 1000.0 / SYSCLOCK_ARM11;
    constexpr auto tick_to_us = 1'000'000.0 / SYSCLOCK_ARM11;
    last_draw_breakdown_ = {
        static_cast<float>((stage_background - stage_begin) * tick_to_ms),
        static_cast<float>((stage_world - stage_background) * tick_to_ms),
        static_cast<float>((stage_finish - stage_world) * tick_to_ms),
        static_cast<std::uint32_t>(
            (stage_world_prepare_end - stage_background) * tick_to_us),
        static_cast<std::uint32_t>(
            (stage_world - stage_world_prepare_end) * tick_to_us),
        static_cast<std::uint32_t>(
            (stage_world_composite_end - stage_world) * tick_to_us),
        static_cast<std::uint32_t>(
            (stage_native_model_end - stage_native_model_begin) * tick_to_us),
        static_cast<std::uint32_t>(
            (stage_finish - stage_native_model_end) * tick_to_us)};
    return wide_frame_;
}

} // namespace starfox::platform_3ds
