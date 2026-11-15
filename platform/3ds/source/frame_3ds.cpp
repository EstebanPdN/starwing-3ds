#include "starfox/render/exterior_ship_visibility.hpp"
#include "starfox/render/source_sprite_material.hpp"
#include "frame_3ds.hpp"
#include "starfox/render/source_depth_tables.hpp"
#include "starfox/render/raster_replay.hpp"
#include "hud_layout.hpp"
#include "shape_memory.hpp"
#include "native_canvas.hpp"
#include "stereo_3ds.hpp"
#include "presentation_3ds.hpp"

#include "starfox/render/palette.hpp"
#include "starfox/render/word_math.hpp"

#include <3ds.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

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
      models_(model_settings()), text_(rom, symbols), dust_(rom, symbols),
      exterior_visibility_(symbols) {
    top_hud_spans_.reserve(512U);
    source_foreground_spans_.reserve(256U);
    items_.reserve(simulation::kMaximumObjects);
    // The scene compiler reads the packed stream; see native_commands().
    native_commands_.set_packed(true);
    resolved_.view_pos_x = ram_symbol("VIEWPOSX");
    resolved_.view_pos_y = ram_symbol("VIEWPOSY");
    resolved_.view_pos_z = ram_symbol("VIEWPOSZ");
    resolved_.view_rot_x = ram_symbol("VIEWROTXW");
    resolved_.view_rot_y = ram_symbol("VIEWROTYW");
    resolved_.view_rot_z = ram_symbol("VIEWROTZW");
    resolved_.bg2_x_scroll = ram_symbol("BG2XSCROLL");
    resolved_.bg2_scroll = ram_symbol("BG2SCROLL");
    resolved_.game_frame = ram_symbol("GAMEFRAME");
    resolved_.shadow_height = ram_symbol("SHADOWHEIGHT");
    resolved_.player_fly_mode = ram_symbol("PLAYERFLYMODE");
    resolved_.vanish_x = fx_symbol("M_VANISHX");
    resolved_.vanish_y = fx_symbol("M_VANISHY");
    resolved_.depth_colours = fx_symbol("M_DEPTHSTAB");
    resolved_.depth_thresholds = fx_symbol("M_DEPTHTABLE");
    const auto first_address = [&symbols](const char* name) {
        const auto& addresses = symbols.find(name);
        return addresses.empty() ? 0U : addresses.front();
    };
    resolved_.flash_player_strategy = first_address("FLASHPLAYER_STRAT");
    resolved_.depth_tables = first_address("DEPTHTABLES");
    resolved_.special_colour = colour_symbol("ID_1_C");
    resolved_.red_colour = colour_symbol("RED_C");
    resolved_.white_colour = colour_symbol("WHITE_C");
}

void Frame3ds::capture_after_tick(const simulation::GameSimulation& game) {
    const auto& map = game.map();
    const auto camera = timing::TransformSnapshot{
        static_cast<std::int16_t>(map.read_native_word(resolved_.view_pos_x)),
        static_cast<std::int16_t>(map.read_native_word(resolved_.view_pos_y)),
        static_cast<std::int16_t>(map.read_native_word(resolved_.view_pos_z)),
        map.read_native_word(resolved_.view_rot_x),
        map.read_native_word(resolved_.view_rot_y),
        map.read_native_word(resolved_.view_rot_z)};
    // The same endpoints as render::capture_object_snapshots, written over
    // the older of the two captures instead of into a new hash map.
    auto& captured = object_snapshots_[1U - current_objects_index_];
    const auto& latest = object_snapshots_[current_objects_index_];
    captured.present.fill(false);
    captured.count = 0U;
    const auto& objects = game.objects();
    for (auto handle = objects.first_active(); handle != 0U;
         handle = objects.next_active(handle)) {
        const auto& object = objects.at(handle);
        // Invisible strategies can leave placeholder coordinates until their
        // first visible update; they are not interpolation endpoints.
        if ((object.strategy_flags[3] & 0x08U) != 0U) continue;
        auto& entry = captured.entries[handle];
        const auto transform = timing::TransformSnapshot{
            object.world_x, object.world_y, object.world_z,
            static_cast<std::uint16_t>(object.rotation_x << 8U),
            static_cast<std::uint16_t>(object.rotation_y << 8U),
            static_cast<std::uint16_t>(object.rotation_z << 8U)};
        // The matrix depends on the three angles alone: an object that did
        // not turn since the last capture keeps the one computed then.
        const auto* held = latest.find(handle);
        if (held != nullptr && held->transform.pitch == transform.pitch
            && held->transform.yaw == transform.yaw
            && held->transform.roll == transform.roll) {
            entry.rotation_matrix = held->rotation_matrix;
        } else {
            entry.rotation_matrix = simulation::transpose_q15(
                simulation::rotation_matrix_q15(trigonometry_,
                    simulation::wrap16(-static_cast<std::int32_t>(transform.pitch)),
                    simulation::wrap16(-static_cast<std::int32_t>(transform.yaw)),
                    simulation::wrap16(-static_cast<std::int32_t>(transform.roll))));
        }
        entry.transform = transform;
        entry.shape = object.shape;
        entry.strategy_address = object.strategy_address;
        entry.type = object.type;
        entry.generation = objects.generation(handle);
        captured.present[handle] = true;
        ++captured.count;
    }
    const bool scene_cut = !snapshots_ready_
        || game.scene_revision() != snapshot_scene_revision_
        || game.flow_state() != snapshot_flow_
        || timing::camera_transform_is_discontinuous(current_camera_, camera);
    bg2_motion_.capture(map.ppu_state(),
        static_cast<std::int16_t>(map.peek_ram_word(resolved_.bg2_x_scroll).value_or(0)),
        static_cast<std::int16_t>(map.peek_ram_word(resolved_.bg2_scroll).value_or(0)),
        map.background(), scene_cut);
    current_objects_index_ = static_cast<std::uint8_t>(1U - current_objects_index_);
    if (!snapshots_ready_) {
        current_camera_ = previous_camera_ = camera;
        previous_objects_are_current_ = true;
        snapshots_ready_ = true;
    } else {
        previous_camera_ = current_camera_;
        current_camera_ = camera;
        previous_objects_are_current_ = false;
        if (game.scene_revision() != snapshot_scene_revision_
            || game.flow_state() != snapshot_flow_
            || timing::camera_transform_is_discontinuous(
                previous_camera_, current_camera_)) {
            previous_camera_ = current_camera_;
            previous_objects_are_current_ = true;
        }
    }
    const auto matrix_for = [this](const timing::TransformSnapshot& pose) {
        return simulation::rotation_matrix_q15(trigonometry_,
            static_cast<std::int16_t>(pose.pitch),
            static_cast<std::int16_t>(pose.yaw),
            static_cast<std::int16_t>(pose.roll));
    };
    previous_view_matrix_ = matrix_for(previous_camera_);
    current_view_matrix_ = matrix_for(current_camera_);
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
        ++shape_cache_generation_;
        ++shape_cache_evictions_;
    }
    return true;
}

const assets::Shape* Frame3ds::load_base_shape(
    std::uint16_t shape, std::uint16_t colour, std::uint16_t minimum_colours) {
    CachedShape* entry = nullptr;
    return resolve_base_shape(shape, colour, minimum_colours, &entry);
}

// *entry is the retained cache entry the result lives in, or null when the
// result is absent or the per-draw transient.
const assets::Shape* Frame3ds::resolve_base_shape(std::uint16_t shape,
    std::uint16_t colour, std::uint16_t minimum_colours, CachedShape** entry) {
    *entry = nullptr;
    const auto variant = std::uint64_t{minimum_colours} << 32U;
    const auto key = variant | (std::uint64_t{shape} << 16U) | colour;
    if (invalid_shapes_.contains(key)) return nullptr;
    auto found = shapes_.find(key);
    if (found == shapes_.end() && colour != 0U) {
        const auto original = shapes_.find(variant | (std::uint64_t{shape} << 16U));
        if (original != shapes_.end() && original->second.shape.header.colour_pointer == colour) {
            original->second.last_use = ++shape_cache_clock_;
            *entry = &original->second;
            return &original->second.shape;
        }
    }
    if (found == shapes_.end()) {
        try {
            auto decoded = decoder_.decode(shape, {}, colour, minimum_colours);
            const auto bytes = shape_storage_bytes(decoded);
            if (!make_shape_cache_room(bytes)) {
                transient_base_shape_ = std::move(decoded);
                return &*transient_base_shape_;
            }
            found = shapes_.emplace(key, CachedShape{std::move(decoded), bytes,
                ++shape_cache_clock_}).first;
            ++shape_cache_generation_;
            shape_cache_bytes_ += bytes;
            ++shape_cache_insertions_;
        } catch (const std::exception&) {
            if (invalid_shapes_.size() >= 4096U) invalid_shapes_.clear();
            invalid_shapes_.insert(key);
            ++shape_cache_generation_;
            return nullptr;
        }
    }
    // References survive unordered_map rehashes when a LOD is inserted.
    found->second.last_use = ++shape_cache_clock_;
    *entry = &found->second;
    return &found->second.shape;
}

const assets::Shape* Frame3ds::load_lod_shape(const assets::Shape& base,
    std::uint16_t pointer, std::uint16_t colour, std::uint16_t minimum_colours) {
    CachedShape* entry = nullptr;
    return resolve_lod_shape(base, pointer, colour, minimum_colours, &entry);
}

const assets::Shape* Frame3ds::resolve_lod_shape(const assets::Shape& base,
    std::uint16_t pointer, std::uint16_t colour, std::uint16_t minimum_colours,
    CachedShape** entry) {
    *entry = nullptr;
    if (pointer == static_cast<std::uint16_t>(base.header.address)) return &base;
    const auto material = colour != 0U ? colour : base.header.colour_pointer;
    // Full headers occupy bank zero in this cartridge; use the spare high
    // byte for the runtime material index, preserving immutable variants.
    const auto key = (std::uint64_t{minimum_colours} << 48U)
        | (std::uint64_t{base.header.address} << 32U)
        | (std::uint64_t{pointer} << 16U) | material;
    if (invalid_lods_.contains(key)) return nullptr;
    auto found = lod_shapes_.find(key);
    if (found == lod_shapes_.end()) {
        try {
            auto decoded = decoder_.decode_lod(base.header, pointer, colour, minimum_colours);
            const auto bytes = shape_storage_bytes(decoded);
            // The caller may use base again after drawing this LOD. Other
            // unordered_map entries can be evicted without moving base.
            if (!make_shape_cache_room(bytes, &base)) {
                transient_lod_shape_ = std::move(decoded);
                return &*transient_lod_shape_;
            }
            found = lod_shapes_.emplace(key, CachedShape{std::move(decoded), bytes,
                ++shape_cache_clock_}).first;
            ++shape_cache_generation_;
            shape_cache_bytes_ += bytes;
            ++shape_cache_insertions_;
        } catch (const std::exception&) {
            if (invalid_lods_.size() >= 4096U) invalid_lods_.clear();
            invalid_lods_.insert(key);
            ++shape_cache_generation_;
            return nullptr;
        }
    }
    found->second.last_use = ++shape_cache_clock_;
    *entry = &found->second;
    return &found->second.shape;
}

// An object slot asks for the same model draw after draw. While the answer
// cannot have changed (same key, no cache entry added or removed since),
// repeat it: the slow path would find the same entry and stamp the same use.
const assets::Shape* Frame3ds::bound_base_shape(ShapeBinding& binding,
    std::uint16_t shape, std::uint16_t colour, std::uint16_t minimum_colours) {
    const auto key = (std::uint64_t{minimum_colours} << 32U)
        | (std::uint64_t{shape} << 16U) | colour;
    if (binding.entry != nullptr && binding.key == key
        && binding.generation == shape_cache_generation_) {
        binding.entry->last_use = ++shape_cache_clock_;
        return &binding.entry->shape;
    }
    CachedShape* entry = nullptr;
    const auto* result = resolve_base_shape(shape, colour, minimum_colours, &entry);
    binding = {key, shape_cache_generation_, entry};
    return result;
}

const assets::Shape* Frame3ds::bound_lod_shape(ShapeBinding& binding,
    const assets::Shape& base, std::uint16_t pointer, std::uint16_t colour,
    std::uint16_t minimum_colours) {
    if (pointer == static_cast<std::uint16_t>(base.header.address)) return &base;
    const auto material = colour != 0U ? colour : base.header.colour_pointer;
    const auto key = (std::uint64_t{minimum_colours} << 48U)
        | (std::uint64_t{base.header.address} << 32U)
        | (std::uint64_t{pointer} << 16U) | material;
    if (binding.entry != nullptr && binding.key == key
        && binding.generation == shape_cache_generation_) {
        binding.entry->last_use = ++shape_cache_clock_;
        return &binding.entry->shape;
    }
    CachedShape* entry = nullptr;
    const auto* result = resolve_lod_shape(base, pointer, colour, minimum_colours, &entry);
    binding = {key, shape_cache_generation_, entry};
    return result;
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
        ++shape_cache_generation_;
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
    model_scratch_.word_projected.reserve(vertices);
    model_scratch_.word_polygon.reserve(polygon + 8U);
    model_scratch_.word_clipping_scratch.reserve(polygon + 8U);
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
        const auto* base = load_base_shape(object.shape, colour, render::source_sprite_colour_count(object));
        if (!base) continue;
        for (auto pointer : {base->header.lod1_pointer, base->header.lod2_pointer,
                 base->header.lod3_pointer, base->header.shadow_pointer})
            static_cast<void>(load_lod_shape(*base, pointer, colour, render::source_sprite_colour_count(object)));
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
        : static_cast<std::int16_t>(map.read_native_word(resolved_.bg2_x_scroll));
    const auto y = ppu.background_mode == 3U ? ppu.bg2_scroll_y
        : static_cast<std::int16_t>(map.read_native_word(resolved_.bg2_scroll));
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
    const auto x = static_cast<std::int16_t>(map.peek_ram_word(resolved_.bg2_x_scroll).value_or(0));
    const auto y = static_cast<std::int16_t>(map.peek_ram_word(resolved_.bg2_scroll).value_or(0));
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
    const auto x = static_cast<std::int16_t>(map.peek_ram_word(resolved_.bg2_x_scroll).value_or(0));
    const auto y = static_cast<std::int16_t>(map.peek_ram_word(resolved_.bg2_scroll).value_or(0));
    if (!bg2_motion_.matches_source(ppu, x, y, map.background())) return ppu;
    // presentation_ppu_ is the live PPU with BG2's roll interpolated. The
    // simulation names the contents of VRAM, CGRAM and OAM with revision
    // tokens (unique per CPU instance, advanced by every write that changes
    // a byte), so those 67 KB are copied again only when a token moved. The
    // registers and HDMA tables after them are small and copied every time.
    using Ppu = simulation::SnesPpuState;
    static_assert(std::is_trivially_copyable_v<Ppu>);
    static_assert(offsetof(Ppu, vram) == 0U
        && offsetof(Ppu, cgram) == sizeof(Ppu::vram)
        && offsetof(Ppu, oam) == offsetof(Ppu, cgram) + sizeof(Ppu::cgram)
        && offsetof(Ppu, background_mode) == offsetof(Ppu, oam) + sizeof(Ppu::oam),
        "the PPU memory arrays must stay first, in this order");
    const auto memory_revision = map.ppu_memory_revision();
    const auto vram_revision = map.ppu_vram_revision();
    if (!presentation_ppu_synced_ || presentation_ppu_vram_revision_ != vram_revision) {
        presentation_ppu_ = ppu;
    } else {
        if (presentation_ppu_memory_revision_ != memory_revision) {
            presentation_ppu_.cgram = ppu.cgram;
            presentation_ppu_.oam = ppu.oam;
        }
        std::memcpy(static_cast<void*>(&presentation_ppu_.background_mode),
            &ppu.background_mode, sizeof(Ppu) - offsetof(Ppu, background_mode));
        // The only VRAM this function writes: restore it before the new roll.
        std::memcpy(presentation_ppu_.vram.data() + Bg2Motion::roll_table_byte,
            ppu.vram.data() + Bg2Motion::roll_table_byte, Bg2Motion::roll_table_bytes);
    }
    presentation_ppu_synced_ = true;
    presentation_ppu_memory_revision_ = memory_revision;
    presentation_ppu_vram_revision_ = vram_revision;
    bg2_motion_.apply(presentation_ppu_, interpolation_alpha);
    return presentation_ppu_;
}

Frame3ds::StorageStats Frame3ds::storage_stats() const noexcept {
    StorageStats result;
    result.bg2_presentation_fixed_bytes = sizeof(bg2_motion_) + sizeof(presentation_ppu_)
        + sizeof(bg2_mode1_atlas_);
    for (const auto* frame : {&frame_, &wide_frame_, &world_, &right_world_,
            &right_frame_, &ex_overlay_, &mode2_background_cache_, &corner_hud_,
            &meter_hud_, &top_hud_cache_, &native_background_, &empty_native_})
        result.framebuffer_capacity_bytes += frame->storage_capacity_bytes();
    result.framebuffer_capacity_bytes += bg2_mode1_atlas_.storage_capacity_bytes();
    for (const auto& cache : mode1_caches_)
        for (const auto* frame : {&cache.bg3_low, &cache.bg3_high,
                &cache.panorama_low, &cache.panorama_high,
                &cache.bg2_low, &cache.bg2_high, &cache.bg1})
            result.framebuffer_capacity_bytes += frame->storage_capacity_bytes();
    const auto bytes = [](const auto& values) {
        return values.capacity() * sizeof(values[0]);
    };
    result.item_capacity_bytes = bytes(items_);
    // Fixed storage of the two object captures; no world-state copy remains.
    result.draw_order_capacity_bytes = 0U;
    result.object_capacity_bytes = 0U;
    result.foreground_spans_size = source_foreground_spans_.size();
    result.foreground_spans_capacity = source_foreground_spans_.capacity();
    result.hud_spans_size = top_hud_spans_.size();
    result.hud_spans_capacity = top_hud_spans_.capacity();
    result.spans_capacity_bytes = bytes(source_foreground_spans_) + bytes(top_hud_spans_);
    result.scratch_capacity_bytes = bytes(model_scratch_.transformed_vertices)
        + bytes(model_scratch_.projected) + bytes(model_scratch_.ordered_faces)
        + bytes(model_scratch_.camera_polygon) + bytes(model_scratch_.camera_clipping_scratch)
        + bytes(model_scratch_.polygon) + bytes(model_scratch_.raster_polygon)
        + bytes(model_scratch_.clipping_scratch)
        + bytes(model_scratch_.word_projected) + bytes(model_scratch_.word_polygon)
        + bytes(model_scratch_.word_clipping_scratch);
    result.interpolation_entries = previous_objects().count + current_objects().count;
    result.interpolation_buckets = 2U * ObjectSnapshots::slots;
    result.fixed_particle_dust_bytes = 0U;
    result.mode1_layer_plan_capacity_bytes=mode1_layer_plan_.storage_capacity_bytes();
    result.tracked_vector_capacity_bytes = result.framebuffer_capacity_bytes
        + result.item_capacity_bytes + result.draw_order_capacity_bytes
        + result.object_capacity_bytes + result.spans_capacity_bytes
        + result.scratch_capacity_bytes + result.mode1_layer_plan_capacity_bytes;
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
    if (!snapshots_ready_) capture_after_tick(game);
    interpolation_alpha = std::clamp(interpolation_alpha, 0.0, 1.0);
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
    retained_mode1_active_=false;
    last_native_world_=native_world;
    const bool cache_mode1_layers = native_world && native_mode1_background;
    const bool cache_source_foreground = (mode2_gameplay || cache_mode1_layers)
        && !map.native_model_draw().active && !game.briefing_state().active;
    // Original Mode 2 has only OBJ and pause text in the source foreground.
    // A BG2 table update or a changing status meter must not redraw it. EX
    // additionally draws BG1 with CGRAM-dependent black rejection, so retain
    // the broader memory token there.
    const bool precise_source_sprites = game.experience()
        == simulation::Experience::original;
    bool retained_mode1=retained_mode1_enabled_ && cache_mode1_layers
        && precise_source_sprites && !map.native_model_draw().active
        && !game.briefing_state().active && !game.paused()
        && Mode1LayerPlan::eligible(ppu);
    if (retained_mode1) {
        if (!mode1_empty_sprite_cache_.matches(ppu,map.ppu_vram_revision()))
            mode1_empty_sprite_cache_.capture(ppu,map.ppu_vram_revision());
        // Status artwork is still drawn by the final HUD pass. Any other
        // sprite selects the established priority compositor for this frame.
        retained_mode1=mode1_empty_sprite_cache_.character_count()==0U;
    }
    const bool reuse_source_foreground = cache_source_foreground
        && !retained_mode1 && !last_retained_mode1_
        && source_foreground_valid_
        && source_foreground_paused_ == game.paused()
        && source_foreground_experience_ == game.experience()
        && last_flow_ == game.flow_state()
        && last_scene_revision_ == game.scene_revision()
        && (cache_mode1_layers
            ? native_mode1_layers_valid_ && last_draw_valid_
                && same_ppu_metadata(last_ppu_, ppu)
                && native_mode1_scroll_x_ == background_x
                && native_mode1_scroll_y_ == background_y
                && last_ppu_memory_revision_ == map.ppu_memory_revision()
            : same_mode2_foreground(last_ppu_, ppu)
        && (last_ppu_memory_revision_ == map.ppu_memory_revision()
            || (precise_source_sprites
                && source_sprite_cache_.matches(ppu, map.ppu_vram_revision()))));
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
        if (retained_mode1 || reuse_source_foreground) return;
        sprites_.draw_objects(ppu, frame_, priority, 0, true, false,
            nullptr, level_flow, nullptr, false, level_flow);
    };
    if (!retained_mode1 && !reuse_source_foreground) frame_.clear(0U);
    // The PICA BG2 path needs exact foreground coverage: palette index zero
    // can be an opaque sprite or model pixel, not just an empty pixel.
    if (mode2_gameplay && !reuse_source_foreground) frame_.begin_write_coverage();
    Mode1BackgroundCache* mode1_cache = nullptr;
    const auto restore_mode1 = [this, reuse_source_foreground, retained_mode1](const render::Framebuffer& layer) {
        if (retained_mode1 || reuse_source_foreground) return;
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
        if (mode1_cache == nullptr && native_mode1_background) {
            // No entry holds this exact frame, as after every source tick
            // that scrolls a level. An entry whose stationary BG3 atlas is
            // still current needs only its scrolled layers refreshed, and it
            // keeps handing the presenter the same atlas under the same
            // name. Rotating to the other entry instead presented an equal
            // atlas under a new name on each tick, which the presenter can
            // only establish by comparing both 1536-pixel planes in full.
            // The second entry stays for a second tile set, as on the title.
            for (auto& candidate : mode1_caches_) {
                if (candidate.valid && candidate.bg2_low.width() == 400U
                    && same_mode1_bg3_tiles(candidate.ppu, ppu)
                    && std::equal(ppu.cgram.begin(), ppu.cgram.begin() + 32U,
                        candidate.cgram.begin())
                    && (candidate.ppu_vram_revision == map.ppu_vram_revision()
                        || candidate.atlas_source.matches(ppu))) {
                    mode1_cache = &candidate;
                    break;
                }
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
        const bool same_bg2 = cache.valid && cache.bg2_raster_valid
            && cache.ppu_vram_revision == map.ppu_vram_revision()
            && same_bg2_tiles(cache.ppu, ppu)
            && cache.scroll_x == background_x
            && cache.scroll_y == background_y
            && (!ppu.tunnel_scene || cache.cgram == ppu.cgram);
        const bool same_atlas = cache.valid
            && same_mode1_bg3_tiles(cache.ppu, ppu)
            && std::equal(ppu.cgram.begin(), ppu.cgram.begin() + 32U,
                cache.cgram.begin())
            && (cache.ppu_vram_revision == map.ppu_vram_revision()
                || (native_mode1_background && cache.atlas_source.matches(ppu)));
        const bool same_bg3 = cache.valid
            && (native_mode1_background ? same_atlas
                : cache.ppu_vram_revision == map.ppu_vram_revision())
            && same_mode1_bg3(cache.ppu, ppu);
        const bool same_panorama = !native_mode1_background || (same_bg3
            && same_atlas && cache.panorama_valid);
        const bool same_background = same_bg1 && same_bg2 && same_bg3 && same_panorama;
        if (same_background) ++background_cache_hits_;
        else ++background_cache_misses_;
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
            cache.atlas_source.capture(ppu);
            if (++panorama_atlas_revision_==0U) panorama_atlas_revision_=1U;
            cache.atlas_revision=panorama_atlas_revision_;
        } else if (!native_mode1_background && !same_bg3) {
            cache.bg3_low.clear();
            cache.bg3_high.clear();
            backgrounds_.draw_bg3_split(ppu,
                cache.bg3_low, cache.bg3_high);
        }
        if (retained_mode1 && !same_panorama) cache.panorama_valid=false;
        if (!retained_mode1 && native_mode1_background && !same_panorama) {
            scroll_native_panorama_atlas(cache.bg3_low, cache.bg3_high,
                cache.panorama_low, cache.panorama_high, ppu);
            cache.panorama_valid = true;
        }
        if (retained_mode1) {
            cache.bg2_raster_valid=false;
            if (!bg2_mode1_atlas_.prepare(backgrounds_,ppu,map.ppu_vram_revision())
                || !mode1_layer_plan_.build(ppu,background_x,background_y,
                    cache.bg3_low,cache.bg3_high,cache.atlas_revision,
                    bg2_mode1_atlas_.low(),bg2_mode1_atlas_.high(),bg2_mode1_atlas_.revision(),
                    map.ppu_memory_revision())) {
                set_retained_mode1_layers(false);
                const auto& fallback=draw(game,gpu_bg2,interpolation_alpha,prepared_background);
                set_retained_mode1_layers(true);
                return fallback;
            }
        } else if (!same_bg2) {
            if(!bg2_mode1_atlas_.draw(backgrounds_,ppu,background_x,background_y,
                cache.bg2_low,cache.bg2_high,native_mode1_background?72:0,native_mode1_background?8:0,
                map.ppu_vram_revision())) {
            cache.bg2_low.clear();
            cache.bg2_high.clear();
            backgrounds_.draw_bg2_mode1_split(ppu, background_x,
                background_y, cache.bg2_low, cache.bg2_high,
                native_mode1_background ? 72 : 0,
                native_mode1_background ? 8 : 0);
            }
            cache.bg2_raster_valid=true;
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

    using render::word::exact_lround;
    const auto camera = timing::interpolate(
        previous_camera_, current_camera_, interpolation_alpha);
    const auto camera_x = static_cast<std::int16_t>(exact_lround(camera.x));
    const auto camera_y = static_cast<std::int16_t>(exact_lround(camera.y));
    const auto camera_z = static_cast<std::int16_t>(exact_lround(camera.z));
    const auto camera_pitch = static_cast<std::int16_t>(
        exact_lround(camera.pitch));
    const auto camera_yaw = static_cast<std::int16_t>(
        exact_lround(camera.yaw));
    const auto camera_roll = static_cast<std::int16_t>(
        exact_lround(camera.roll));
    const auto vanish_x = static_cast<std::int16_t>(
        map.read_native_word(resolved_.vanish_x));
    const auto vanish_y = static_cast<std::int16_t>(
        map.read_native_word(resolved_.vanish_y));
    const auto frame_number = static_cast<std::uint8_t>(
        map.read_native_byte(resolved_.game_frame) & 0x7fU);
    const auto shadow_height = static_cast<std::int16_t>(
        map.read_native_word(resolved_.shadow_height));
    const auto player_fly_mode = map.read_native_byte(resolved_.player_fly_mode);
    const auto depth_colours = map.read_native_word(resolved_.depth_colours);
    const auto depth_thresholds = map.read_native_word(resolved_.depth_thresholds);
    const auto dots_mode = map.dots_mode();
    const auto dust_count = game.dust_point_count();
    const auto& draw_order = game.draw_order();
    const auto world_cache_validation_begin = svcGetSystemTick();
    // Between two source ticks the composed model matrices saturate instead
    // of wrapping (see compose_model_view_matrix).
    const bool blended = interpolation_alpha > 0.0 && interpolation_alpha < 1.0;
    const auto view = interpolation_alpha >= 1.0
        ? current_view_matrix_
        : simulation::interpolate_rotation_matrix_q15(
            previous_view_matrix_, current_view_matrix_, interpolation_alpha);
    const timing::RenderTransform raster_camera{
        static_cast<double>(camera_x), static_cast<double>(camera_y),
        static_cast<double>(camera_z), static_cast<double>(camera_pitch),
        static_cast<double>(camera_yaw), static_cast<double>(camera_roll)};
    const auto flash_player_strategy = resolved_.flash_player_strategy;
    const auto& objects = game.objects();
    const auto& objects_now = current_objects();
    const auto& objects_before = previous_objects();
    // timing::interpolate's word, rounded as the frame rounds it. The two
    // ends are exact, so only a true in-between costs floating point.
    const auto interpolated_word = [interpolation_alpha](std::int32_t from, std::int32_t to) {
        auto delta = static_cast<std::int64_t>(to) - from;
        if (delta > 32'767) delta -= 65'536;
        else if (delta < -32'768) delta += 65'536;
        // Source words differ by less than 2^17: convert the difference as
        // a 32-bit integer (one VFP instruction) whenever it is one. The
        // 64-bit conversion, a library call on the ARM11, yields the same
        // double for the same integer.
        const auto narrow = static_cast<std::int32_t>(delta);
        const double step = narrow == delta ? static_cast<double>(narrow)
            : static_cast<double>(delta);
        return static_cast<std::int16_t>(exact_lround(static_cast<double>(from)
            + step * interpolation_alpha));
    };
    const auto same_entity = [](const render::ObjectPresentationSnapshot& a,
                                const render::ObjectPresentationSnapshot& b) {
        return a.generation == b.generation && a.shape == b.shape
            && a.type == b.type && a.strategy_address == b.strategy_address;
    };
    // Parts of one craft share a rotation (the flashing overlay borrows the
    // Arwing's): keep the last interpolated pair instead of repeating it.
    const simulation::MatrixQ15* blended_from = nullptr;
    const simulation::MatrixQ15* blended_to = nullptr;
    simulation::MatrixQ15 blended_matrix{};
    items_.clear();
    items_.reserve(draw_order.size());
    for (const auto handle : draw_order) {
        if (!objects.is_active(handle)) continue;
        const auto& object = objects.at(handle);
        if ((object.strategy_flags[3] & 0x08U) != 0U || object.shape == 0U)
            continue;
        std::int16_t world_x{}, world_y{}, world_z{};
        simulation::MatrixQ15 object_matrix;
        if (const auto* current = objects_now.find(handle)) {
            const auto* before = objects_before.find(handle);
            const auto* from = before != nullptr && same_entity(*before, *current)
                ? before : current;
            const auto* to = current;
            if (flash_player_strategy != 0U
                && object.strategy_address == flash_player_strategy) {
                // This alternating NULLSHAPE/wireframe is attached to the
                // Arwing. A shape change must not hold the effect at the next
                // source pose while the player's geometry is interpolating.
                // Preserve its source blink/material state; borrow only the
                // owner's position and orientation history
                // (render::anchor_player_overlay).
                if (const auto* owner = objects_now.find(game.player())) {
                    const auto* old = objects_before.find(game.player());
                    from = old != nullptr && same_entity(*old, *owner) ? old : owner;
                    to = owner;
                }
            }
            world_x = interpolated_word(from->transform.x, to->transform.x);
            world_y = interpolated_word(from->transform.y, to->transform.y);
            world_z = interpolated_word(from->transform.z, to->transform.z);
            // simulation::interpolate_rotation_matrix_q15 returns an end, or
            // the shared matrix of two equal ends, before doing any work.
            if (interpolation_alpha <= 0.0) object_matrix = from->rotation_matrix;
            else if (interpolation_alpha >= 1.0
                || from->rotation_matrix == to->rotation_matrix)
                object_matrix = to->rotation_matrix;
            else {
                if (blended_from == nullptr
                    || *blended_from != from->rotation_matrix
                    || *blended_to != to->rotation_matrix) {
                    blended_matrix = simulation::interpolate_rotation_matrix_q15(
                        from->rotation_matrix, to->rotation_matrix, interpolation_alpha);
                    blended_from = &from->rotation_matrix;
                    blended_to = &to->rotation_matrix;
                }
                object_matrix = blended_matrix;
            }
        } else {
            // No captured endpoint: the live pose, as the cartridge has it.
            world_x = object.world_x;
            world_y = object.world_y;
            world_z = object.world_z;
            object_matrix = simulation::transpose_q15(
                simulation::rotation_matrix_q15(trigonometry_,
                    simulation::wrap16(-static_cast<std::int32_t>(
                        static_cast<std::uint16_t>(object.rotation_x) << 8U)),
                    simulation::wrap16(-static_cast<std::int32_t>(
                        static_cast<std::uint16_t>(object.rotation_y) << 8U)),
                    simulation::wrap16(-static_cast<std::int32_t>(
                        static_cast<std::uint16_t>(object.rotation_z) << 8U))));
        }
        const auto position = render::word::transform_q15(view, {
            render::word::subtract16(world_x, camera_x),
            render::word::subtract16(world_y, camera_y),
            render::word::subtract16(world_z, camera_z)});
        items_.push_back({handle, position, object_matrix, world_x, world_z, &object});
    }
    world_cache_validation_us_ = static_cast<std::uint64_t>(
        (svcGetSystemTick() - world_cache_validation_begin)
            * (1000000.0 / SYSCLOCK_ARM11));
    world_cache_snapshot_us_ = 0U;
    const auto stage_world_prepare_end = svcGetSystemTick();
    std::uint64_t world_record_ticks=0, scene_compile_ticks=0;
    {
        ++world_cache_misses_;
        const auto world_record_begin=svcGetSystemTick();
        const auto depth_address = resolved_.depth_tables;
        const auto special_colour = resolved_.special_colour;
        const auto red_colour = resolved_.red_colour;
        const auto white_colour = resolved_.white_colour;
        const auto& exterior_visibility = exterior_visibility_;
        const auto player_strategy = objects.is_active(game.player())
            ? objects.at(game.player()).strategy_address : 0U;
        const auto effective_colour = [&](const auto& object) {
            const auto flags = object.strategy_flags[0];
            if ((flags & 0x40U) != 0U) return std::uint16_t{};
            if ((flags & 0x02U) != 0U && (flags & 0x20U) == 0U)
                return static_cast<std::uint16_t>(
                    (flags & 0x01U) != 0U ? red_colour : white_colour);
            return static_cast<std::uint16_t>(
                (flags & 0x01U) != 0U ? special_colour : object.colour_table);
        };
        // The scene's depth-cue tables: ROM constants chosen by three words.
        if (!depth_tables_ || depth_tables_address_ != depth_address
            || depth_tables_thresholds_ != depth_thresholds
            || depth_tables_colours_ != depth_colours) {
            depth_tables_.emplace(rom_, depth_address, depth_thresholds, depth_colours);
            depth_tables_address_ = depth_address;
            depth_tables_thresholds_ = depth_thresholds;
            depth_tables_colours_ = depth_colours;
        }
        auto& source_depth_tables = *depth_tables_;

        for (unsigned side = 0U; side < (stereo ? 2U : 1U); ++side) {
        auto& eye_world = side == 0U ? world_ : right_world_;
        if (native_world) {
            // A recording raster stores no pixel and nothing reads the ones
            // it holds: every consumer of world_ below either follows a
            // software pass, which clears it first, or replays the commands
            // into it, which clears it too. Its 96 KB are left alone.
            native_commands_.reset(400,240);
            native_commands_.accept_edge_runs(true);
            eye_world.record_to(&native_commands_);
        } else eye_world.clear(0U);
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

        const auto make_pose = [&](const RenderItem& item, bool shadow) {
            const auto& object = *item.object;
            const bool true_colour_shadow =
                (object.strategy_flags[0] & 0x04U) != 0U;
            auto position = item.position;
            if (shadow && !true_colour_shadow) {
                position = render::word::transform_q15(view, {
                    render::word::subtract16(item.world_x, camera_x),
                    render::word::subtract16(shadow_height, camera_y),
                    render::word::subtract16(item.world_z, camera_z)});
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
            pose.rotation_matrix = blended
                ? render::word::multiply_presentation_matrix_q15(object_matrix, view)
                : render::word::multiply_matrix_q15(object_matrix, view);
            pose.use_rotation_matrix = true;
            pose.animation_frame = display_frame(object.animation_frame);
            pose.colour_frame = display_frame(object.colour_frame);
            pose.explosion_progress = (object.flags & 0x01U) != 0U
                ? object.count : 0U;
            pose.texture_scroll_x = object.texture_scroll_x;
            pose.texture_scroll_y = object.texture_scroll_y;
            if (depth_address != 0U)
                source_depth_tables.apply(object.extended[21], pose);
            apply_stereo_eye(pose, eye);
            return pose;
        };

        if ((player_fly_mode & 0x08U) != 0U) {
            for (const auto& item : items_) {
                const auto& object = *item.object;
                if (exterior_visibility.hide(player_strategy, object.strategy_address)) continue;
                if ((object.strategy_flags[0] & 0x0cU) == 0U) continue;
                const auto colour = effective_colour(object);
                const auto colour_count = render::source_sprite_colour_count(object);
                const auto* base = bound_base_shape(base_bindings_[item.handle],
                    object.shape, colour, colour_count);
                if (base == nullptr) continue;
                const auto pointer = base->header.shadow_pointer;
                const auto* shape = bound_lod_shape(shadow_bindings_[item.handle],
                    *base, pointer, colour, colour_count);
                if (!shape) continue;
                models_.draw(*shape, make_pose(item, true), eye_world,
                    false, nullptr, nullptr, &model_scratch_);
            }
        }

        for (const auto& item : items_) {
            const auto& object = *item.object;
            if (exterior_visibility.hide(player_strategy, object.strategy_address)) continue;
            if ((object.strategy_flags[0] & 0x04U) != 0U) continue;
            const auto colour = effective_colour(object);
            const auto colour_count = render::source_sprite_colour_count(object);
            // Precaching is optional. Intro, GAME OVER and later spawns can
            // reach this path with a cold cache and must still draw.
            const auto* base = bound_base_shape(base_bindings_[item.handle],
                object.shape, colour, colour_count);
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
            const auto* shape = bound_lod_shape(lod_bindings_[item.handle],
                *base, pointer, colour, colour_count);
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
                false, nullptr, nullptr, &model_scratch_);
        }
        eye_world.record_to(nullptr);
        } // Both eyes share decoded shapes, poses and source simulation.
        const auto compile_begin=svcGetSystemTick();
        world_record_ticks+=compile_begin-world_record_begin;
        const bool scene_ok=!native_world || native_scene_.compile(native_commands_);
        scene_compile_ticks+=svcGetSystemTick()-compile_begin;
        if (!scene_ok) {
            native_commands_.unpack();
            render::replay_raster_commands(native_commands_,world_,nullptr);
        }
    }

    native_scene_active_=native_world && !native_scene_.stats().overflow;
    if (cache_mode1_layers && !native_scene_active_ && (reuse_source_foreground || retained_mode1)) {
        // A retained Mode 1 frame contains only the post-world plane. If
        // this world exceeds the GPU batch, rebuild the complete CPU frame;
        // composing it with the retained foreground would lose lower layers.
        const auto mode1 = native_mode1_enabled_;
        set_native_engine(false);
        const auto& fallback = draw(game, false, interpolation_alpha, prepared_background);
        set_native_engine(true, mode1);
        return fallback;
    }
    const auto stage_world = svcGetSystemTick();
    // The wider world raster already uses full PPU coordinates. Preserve
    // projected geometry that crosses the original Super FX viewport edge.
    render::LayerCompositeSettings world_to_source{};
    world_to_source.offset_x = -72;
    world_to_source.offset_y = -8;
    retained_mode1_active_=retained_mode1 && native_scene_active_;
    if (retained_mode1_active_) {
        // PICA samples the retained layers around the native world. No
        // 256x224 intermediate or 400x240 background is assembled here.
    } else if (native_scene_active_ && native_mode1_background) {
        if (!reuse_source_foreground) {
        if (native_background_.width()!=400U) {
            native_background_=render::Framebuffer(400,240);
        }
        if(!compose_native_mode1_background(frame_,mode1_cache->bg2_low,
            mode1_cache->panorama_low,mode1_cache->panorama_high,native_background_,!ppu.bg3_high_priority)) {
        if (empty_native_.width()!=400U) empty_native_=render::Framebuffer(400,240);
        compose_native_mode1(frame_,empty_native_,mode1_cache->bg2_low,empty_native_,
            mode1_cache->panorama_low,ppu.bg3_high_priority?empty_native_:mode1_cache->panorama_high,
            native_background_,false);
        }
        frame_.begin_write_coverage();
        if (++native_background_revision_ == 0U) native_background_revision_ = 1U;
        }
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
                    false, nullptr, nullptr, &model_scratch_);
            }
    }
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
    const bool top_hud_shown = level_flow && show_top_hud_;
    // Bring the retained top HUD up to this draw's meters and status sprites
    // before choosing how to compose the wide frame: a group that changed
    // names the rectangles it rewrote, and nothing else moved in that layer.
    const bool top_hud_changed = top_hud_shown && refresh_top_hud(ppu,
        game.meter_state(), game.experience(), map.ppu_memory_revision(),
        map.ppu_vram_revision());
    // Over a PICA world the wide frame holds only the 2D layer: the source
    // foreground and the top HUD. Both follow the cartridge, at 20 Hz, while
    // the world under them moves at every presentation. When this draw would
    // compose the layer the wide frame already holds, leave it there.
    const bool wide_foreground_retained = wide_foreground_valid_ && last_draw_valid_
        && native_scene_active_ && (mode2_gameplay || native_mode1_background) && !stereo
        && (reuse_source_foreground || (retained_mode1_active_ && last_retained_mode1_))
        && wide_foreground_top_hud_ == top_hud_shown;
    const bool keep_wide_foreground = wide_foreground_retained && !top_hud_changed;
    // An in-place update writes the frame's coverage beside its pixels.
    const bool wide_frame_editable = !wide_frame_.layer_tags_enabled()
        && wide_frame_.write_coverage().size() == wide_frame_.pixels().size();
    // The same layer with a HUD group redrawn: rewrite those rectangles of
    // the wide frame in place. Mode 1 with source sprites recomposes instead;
    // its layer also holds BG2/BG3 priority planes and follows every tick.
    const bool patch_wide_foreground = wide_foreground_retained && top_hud_changed
        && !top_hud_damage_all_ && (mode2_gameplay || retained_mode1_active_)
        && wide_frame_editable;
    const bool wide_foreground_in_place = keep_wide_foreground || patch_wide_foreground;
    // Mode 2's layer is empty but for the source foreground's spans and the
    // HUD. When the source sprites moved, take the spans it still shows out
    // and put the new ones in, instead of clearing and covering 192 KB.
    const bool replace_wide_source = mode2_gameplay && !wide_foreground_in_place
        && wide_source_layer_ && last_draw_valid_ && native_scene_active_ && !stereo
        && wide_foreground_top_hud_ == top_hud_shown
        && !(top_hud_shown && top_hud_damage_all_) && wide_frame_editable;
    if (mode2_gameplay) {
        frame_.end_write_coverage();
        if (replace_wide_source) erase_wide_source_layer(top_hud_changed);
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
        if (wide_foreground_in_place || replace_wide_source) {}
        else if (native_scene_active_) { wide_frame_.clear(); wide_frame_.begin_write_coverage(); }
        else if (gpu_bg2) wide_frame_.assign_transparent_layer(world_, true);
        else render::composite_transparent_layer(world_, wide_frame_, {});
        if (replace_wide_source) draw_wide_source_layer();
        else if (!wide_foreground_in_place)
            overlay_source_coverage_on_native(frame_, wide_frame_, source_foreground_spans_);
        if (stereo) {
            if (gpu_bg2) right_frame_.assign_transparent_layer(right_world_, true);
            else render::composite_transparent_layer(right_world_, right_frame_, {});
            overlay_source_coverage_on_native(frame_, right_frame_, source_foreground_spans_);
        }
    } else if (retained_mode1_active_) {
        if (!wide_foreground_in_place) {
            wide_frame_.clear(); wide_frame_.begin_write_coverage();
        }
    } else if (native_mode1_background && native_scene_active_) {
        if (!keep_wide_foreground) {
        wide_frame_.clear(); wide_frame_.begin_write_coverage();
        render::composite_transparent_layer(mode1_cache->bg2_high,wide_frame_,{});
        if (ppu.bg3_high_priority)
            render::composite_transparent_layer(mode1_cache->panorama_high,wide_frame_,{});
        frame_.end_write_coverage();
        if (!reuse_source_foreground)
            capture_native_coverage_spans(frame_,source_foreground_spans_);
        overlay_source_coverage_on_native(frame_,wide_frame_,source_foreground_spans_);
        }
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
    if (top_hud_shown) {
        if (top_hud_changed) ++top_hud_cache_misses_;
        else {
            ++top_hud_cache_hits_;
            if(top_hud_ppu_memory_revision_!=map.ppu_memory_revision())
                ++top_hud_selective_hits_;
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
        if (patch_wide_foreground) patch_wide_top_hud(mode2_gameplay);
        else if (replace_wide_source) draw_wide_top_hud();
        else if (!keep_wide_foreground) overlay_hud(wide_frame_);
        if (stereo) overlay_hud(right_frame_);
    }
    if ((mode2_gameplay && gpu_bg2) || native_scene_active_) wide_frame_.end_write_coverage();
    if (!keep_wide_foreground) {
        if (++wide_foreground_revision_ == 0U) wide_foreground_revision_ = 1U;
    }
    wide_foreground_valid_ = native_scene_active_ && (mode2_gameplay || native_mode1_background) && !stereo;
    wide_foreground_top_hud_ = top_hud_shown;
    // Kept, patched, replaced or composed: Mode 2's layer now shows exactly
    // source_foreground_spans_ under the HUD.
    wide_source_layer_ = wide_foreground_valid_ && mode2_gameplay;
    if (stereo && gpu_bg2) right_frame_.end_write_coverage();
    last_draw_stereo_ = stereo;
    if (mode2_gameplay && cache_source_foreground && !reuse_source_foreground && precise_source_sprites)
        source_sprite_cache_.capture(ppu, map.ppu_vram_revision());
    native_mode1_layers_valid_ = cache_mode1_layers && native_scene_active_;
    last_retained_mode1_=retained_mode1_active_;
    native_mode1_scroll_x_ = background_x;
    native_mode1_scroll_y_ = background_y;
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
            (stage_finish - stage_native_model_end) * tick_to_us),
        static_cast<std::uint32_t>(world_record_ticks*tick_to_us),
        static_cast<std::uint32_t>(scene_compile_ticks*tick_to_us)};
    return wide_frame_;
}

namespace {

constexpr int top_hud_width = 400, top_hud_height = 240;

constexpr bool has_area(const HudRect& area) noexcept {
    return area.w > 0 && area.h > 0;
}

constexpr bool same_area(const HudRect& a, const HudRect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// The part of an area inside a window; no area when they do not meet.
constexpr HudRect area_inside(const HudRect& area, const HudRect& window) noexcept {
    const int left = std::max(area.x, window.x);
    const int top = std::max(area.y, window.y);
    const int right = std::min(area.x + area.w, window.x + window.w);
    const int bottom = std::min(area.y + area.h, window.y + window.h);
    return left < right && top < bottom
        ? HudRect{left, top, right - left, bottom - top} : HudRect{};
}

constexpr HudRect top_hud_screen{0, 0, top_hud_width, top_hud_height};
// corner_hud_ and meter_hud_: the cartridge's 256x224 raster.
constexpr HudRect status_layer{0, 0, 256, 224};

constexpr HudRect meter_area(const render::MeterRectangle& rectangle) noexcept {
    return {rectangle.x, rectangle.y, rectangle.width, rectangle.height};
}

void fill_status_layer(render::Framebuffer& layer, const HudRect& area,
    std::uint8_t colour) noexcept {
    const auto stride = layer.stored_width();
    auto* row = layer.pixels().data()
        + static_cast<std::size_t>(area.y) * stride + area.x;
    for (int y = 0; y < area.h; ++y, row += stride)
        std::memset(row, colour, static_cast<std::size_t>(area.w));
}

// The meter layer inside a window is its rectangles that reach the window,
// painted in order. Two lists that agree on those hold the same pixels there.
bool same_meters_inside(const render::MeterRectangles& a,
    const render::MeterRectangles& b, const HudRect& window) noexcept {
    std::size_t i = 0U, j = 0U;
    const auto next = [&window](const render::MeterRectangles& list,
        std::size_t& index, HudRect& part, std::uint8_t& colour) {
        while (index < list.count) {
            const auto& rectangle = list.rectangles[index++];
            part = area_inside(meter_area(rectangle), window);
            if (has_area(part)) { colour = rectangle.colour; return true; }
        }
        return false;
    };
    for (;;) {
        HudRect part_a{}, part_b{};
        std::uint8_t colour_a{}, colour_b{};
        const bool more_a = next(a, i, part_a, colour_a);
        const bool more_b = next(b, j, part_b, colour_b);
        if (more_a != more_b) return false;
        if (!more_a) return true;
        if (!same_area(part_a, part_b) || colour_a != colour_b) return false;
    }
}

// Every meter rectangle is opaque (colour 112 and up), so the box around the
// layer's non-zero pixels in a window is the box around those rectangles.
HudRect meter_bounds(const render::MeterRectangles& list,
    const HudRect& window) noexcept {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
    for (std::size_t i = 0U; i < list.count; ++i) {
        const auto part = area_inside(meter_area(list.rectangles[i]), window);
        if (!has_area(part)) continue;
        if (!any) {
            left = part.x; top = part.y;
            right = part.x + part.w; bottom = part.y + part.h;
            any = true;
        } else {
            left = std::min(left, part.x); top = std::min(top, part.y);
            right = std::max(right, part.x + part.w);
            bottom = std::max(bottom, part.y + part.h);
        }
    }
    return any ? HudRect{left, top, right - left, bottom - top} : HudRect{};
}

std::uint32_t load_pixels4(const std::uint8_t* bytes) noexcept {
    std::uint32_t word;
    std::memcpy(&word, bytes, sizeof(word));
    return word;
}

// The box around a status layer's non-zero pixels in a window.
HudRect layer_bounds(const render::Framebuffer& layer,
    const HudRect& window) noexcept {
    const auto stride = layer.stored_width();
    const auto* pixels = layer.pixels().data();
    const int end = window.x + window.w;
    int left = end, right = window.x - 1, top = -1, bottom = -1;
    for (int y = window.y; y < window.y + window.h; ++y) {
        const auto* row = pixels + static_cast<std::size_t>(y) * stride;
        int x = window.x;
        while (x + 4 <= end && load_pixels4(row + x) == 0U) x += 4;
        while (x < end && row[x] == 0U) ++x;
        if (x == end) continue;
        int last = end - 1;
        while (row[last] == 0U) --last;
        left = std::min(left, x);
        right = std::max(right, last);
        if (top < 0) top = y;
        bottom = y;
    }
    return top < 0 ? HudRect{}
        : HudRect{left, top, right - left + 1, bottom - top + 1};
}

constexpr int scaled_hud(int length, bool wide) noexcept {
    return wide ? (length * 3 + 1) / 2 : length;
}

// Point-sample a crop of a status layer into the top HUD layer at 3:2 (wide)
// or 1:1; index zero is transparent. Returns the area it occupies, unclipped.
HudRect place_hud_crop(const render::Framebuffer& source, const HudRect& crop,
    int x, int y, bool wide, render::Framebuffer& target) noexcept {
    const HudRect placed{x, y, scaled_hud(crop.w, wide), scaled_hud(crop.h, wide)};
    const auto visible = area_inside(placed, top_hud_screen);
    if (!has_area(visible)) return placed;
    std::array<std::uint16_t, top_hud_width> columns;
    for (int column = 0; column < visible.w; ++column) {
        const int dx = visible.x - x + column;
        columns[static_cast<std::size_t>(column)] = static_cast<std::uint16_t>(
            crop.x + (wide ? dx * 2 / 3 : dx));
    }
    const auto stride = source.stored_width();
    const auto* from = source.pixels().data();
    auto* to = target.pixels().data();
    for (int row = 0; row < visible.h; ++row) {
        const int dy = visible.y - y + row;
        const auto* in = from
            + static_cast<std::size_t>(crop.y + (wide ? dy * 2 / 3 : dy)) * stride;
        auto* out = to + static_cast<std::size_t>(visible.y + row) * top_hud_width
            + visible.x;
        for (int column = 0; column < visible.w; ++column) {
            const auto pixel = in[columns[static_cast<std::size_t>(column)]];
            if (pixel != 0U) out[column] = pixel;
        }
    }
    return placed;
}

// Runs of non-zero pixels in [first, last), in raster order.
template<class Emit>
void scan_hud_runs(const std::uint8_t* pixels, std::uint32_t first,
    std::uint32_t last, Emit&& emit) {
    for (auto index = first; index < last;) {
        while (index + 4U <= last && load_pixels4(pixels + index) == 0U) index += 4U;
        while (index < last && pixels[index] == 0U) ++index;
        const auto begin = index;
        while (index < last && pixels[index] != 0U) ++index;
        if (index != begin) emit(NativeCoverageSpan{begin, begin, index - begin});
    }
}

// The pieces of sorted, non-overlapping target spans inside an area of the
// 400-pixel-wide frame: copy(target offset, source offset, count).
template<class Copy>
void for_each_span_inside(std::span<const NativeCoverageSpan> spans,
    const HudRect& area, Copy&& copy) {
    const auto width = static_cast<std::uint32_t>(top_hud_width);
    const auto left = static_cast<std::uint32_t>(area.x);
    const auto right = static_cast<std::uint32_t>(area.x + area.w);
    const auto first_row = static_cast<std::uint32_t>(area.y);
    const auto last_row = static_cast<std::uint32_t>(area.y + area.h);
    const auto begin = first_row * width + left;
    const auto end = (last_row - 1U) * width + right;
    auto span = std::partition_point(spans.begin(), spans.end(),
        [begin](const NativeCoverageSpan& candidate) {
            return candidate.target_offset + candidate.count <= begin;
        });
    for (; span != spans.end() && span->target_offset < end; ++span) {
        auto offset = span->target_offset;
        const auto stop = offset + span->count;
        while (offset < stop) {
            const auto row = offset / width;
            const auto row_stop = std::min(stop, (row + 1U) * width);
            if (row >= first_row && row < last_row) {
                const auto from = std::max(offset, row * width + left);
                const auto to = std::min(row_stop, row * width + right);
                if (from < to)
                    copy(from, span->source_offset + (from - span->target_offset),
                        to - from);
            }
            offset = row_stop;
        }
    }
}

} // namespace

void Frame3ds::add_top_hud_damage(HudRect area) noexcept {
    area = area_inside(area, top_hud_screen);
    if (!has_area(area)) return;
    for (std::size_t i = 0U; i < top_hud_damage_count_; ++i)
        if (same_area(top_hud_damage_[i], area)) return;
    if (top_hud_damage_count_ == top_hud_damage_.size()) {
        top_hud_damage_all_ = true;
        return;
    }
    top_hud_damage_[top_hud_damage_count_++] = area;
}

// Bring top_hud_cache_ and its spans to the HUD of these meters and status
// sprites. Returns whether a pixel of it was rewritten; top_hud_damage_ then
// lists where, or top_hud_damage_all_ says the whole layer was built again.
bool Frame3ds::refresh_top_hud(const simulation::SnesPpuState& ppu,
    const simulation::MeterState& meters, simulation::Experience experience,
    std::uint64_t ppu_memory_revision, std::uint64_t ppu_vram_revision) {
    const bool objects_enabled = (ppu.main_screen & 0x10U) != 0U;
    const bool invalid = !top_hud_cache_valid_ || top_hud_experience_ != experience;
    // Status artwork: OAM records and their characters, or how they decode.
    const bool source_sprites_changed = invalid
        || top_hud_object_select_ != ppu.object_select
        || top_hud_objects_enabled_ != objects_enabled
        || (top_hud_ppu_memory_revision_ != ppu_memory_revision
            && !hud_sprite_cache_.matches(ppu, ppu_vram_revision));
    const bool source_meters_changed = invalid || !(top_hud_meters_ == meters);
    top_hud_damage_count_ = 0U;
    top_hud_damage_all_ = false;
    if (!source_sprites_changed && !source_meters_changed) return false;
    // Groups that overlapped were drawn over each other in order; only the
    // complete rebuild reproduces that.
    const bool rebuild = invalid || !top_hud_disjoint_;
    const bool sprites_changed = rebuild || source_sprites_changed;
    const bool meters_changed = rebuild || source_meters_changed;
    top_hud_damage_all_ = rebuild;

    const bool wide = wide_view_;
    const bool ex = experience == simulation::Experience::starfox_ex;
    const int hud_left = wide ? 8 : 80, hud_right = wide ? 392 : 320;
    const int hud_top = wide ? 8 : 16, hud_bottom = wide ? 232 : 224;
    const HudRect boss_window{96, 0, 160, 40};
    const HudRect shield_window{0, 160, 112, 64};
    const HudRect bombs_window{144, 160, 112, 64};

    // All groups consume the same synthetic meter layer. Keep it equal to a
    // cleared layer with these rectangles painted, and note which group's
    // window sees different pixels.
    const auto rectangles = render::meter_rectangles(meters, meter_hud_.width());
    bool boss_meter_changed = rebuild, shield_meter_changed = rebuild;
    bool bombs_meter_changed = rebuild;
    if (rebuild) {
        top_hud_cache_.clear(0U);
        meter_hud_.clear(0U);
    } else if (meters_changed) {
        boss_meter_changed = !same_meters_inside(
            top_hud_meter_rectangles_, rectangles, boss_window);
        shield_meter_changed = !same_meters_inside(
            top_hud_meter_rectangles_, rectangles, shield_window);
        bombs_meter_changed = !same_meters_inside(
            top_hud_meter_rectangles_, rectangles, bombs_window);
        for (std::size_t i = 0U; i < top_hud_meter_rectangles_.count; ++i) {
            const auto part = area_inside(
                meter_area(top_hud_meter_rectangles_.rectangles[i]), status_layer);
            if (has_area(part)) fill_status_layer(meter_hud_, part, 0U);
        }
    }
    if (meters_changed) {
        for (std::size_t i = 0U; i < rectangles.count; ++i) {
            const auto part = area_inside(
                meter_area(rectangles.rectangles[i]), status_layer);
            if (has_area(part))
                fill_status_layer(meter_hud_, part, rectangles.rectangles[i].colour);
        }
        top_hud_meter_rectangles_ = rectangles;
    }

    // Source OAM labels/icons and synthetic bars have opposite vertical
    // order in the cartridge. Keep them as independent layers so the
    // scaled top HUD always shows the label/icons ABOVE the bar.
    const auto draw_sprites = [&](render::HudElement element, const HudRect& window) {
        // Only this window of the scratch layer is read back.
        fill_status_layer(corner_hud_, window, 0U);
        sprites_.draw_objects(ppu, corner_hud_, std::nullopt, 0, false,
            false, nullptr, false, &meters, true, true, element);
    };
    const auto erase = [&](HudRect& target) {
        const auto visible = area_inside(target, top_hud_screen);
        target = {};
        if (rebuild || !has_area(visible)) return;
        auto* row = top_hud_cache_.pixels().data()
            + static_cast<std::size_t>(visible.y) * top_hud_width + visible.x;
        for (int y = 0; y < visible.h; ++y, row += top_hud_width)
            std::memset(row, 0, static_cast<std::size_t>(visible.w));
        add_top_hud_damage(visible);
    };
    const auto place = [&](const render::Framebuffer& layer, const HudRect& crop,
        int x, int y) {
        const auto placed = place_hud_crop(layer, crop, x, y, wide, top_hud_cache_);
        add_top_hud_damage(placed);
        return placed;
    };

    if (sprites_changed) {
        auto& lives = top_hud_groups_[0];
        erase(lives.upper_target);
        const HudRect window{0, ex ? 160 : 0, 112, ex ? 64 : 40};
        draw_sprites(render::HudElement::lives, window);
        lives.upper = layer_bounds(corner_hud_, window);
        if (has_area(lives.upper))
            lives.upper_target = place(corner_hud_, lives.upper, hud_left, hud_top);
    }

    // The ENEMY label is placed from the meter's width, and both share one crop.
    if (sprites_changed || boss_meter_changed
        || meters.boss_max_health != top_hud_meters_.boss_max_health
        || meters.enabled != top_hud_meters_.enabled) {
        auto& boss = top_hud_groups_[1];
        erase(boss.upper_target);
        boss.upper = {};
        if (meters.boss_max_health != 0U) {
            draw_sprites(render::HudElement::boss_health, boss_window);
            for (std::size_t i = 0U; i < rectangles.count; ++i) {
                const auto part = area_inside(
                    meter_area(rectangles.rectangles[i]), boss_window);
                if (has_area(part))
                    fill_status_layer(corner_hud_, part, rectangles.rectangles[i].colour);
            }
            boss.upper = layer_bounds(corner_hud_, boss_window);
            if (has_area(boss.upper))
                boss.upper_target = place(corner_hud_, boss.upper,
                    hud_right - scaled_hud(boss.upper.w, wide), hud_top);
        }
    }

    const auto stack = [&](TopHudGroup& group, render::HudElement element,
        const HudRect& window, bool right, bool meter_changed) {
        if (!sprites_changed && !meter_changed) return;
        bool sprites_drawn = false;
        if (sprites_changed) {
            draw_sprites(element, window);
            group.upper = layer_bounds(corner_hud_, window);
            sprites_drawn = true;
        }
        if (meter_changed) group.bar = meter_bounds(rectangles, window);
        const auto& upper = group.upper;
        const auto& bar = group.bar;
        HudRect upper_target{}, bar_target{};
        if (!has_area(upper)) {
            if (has_area(bar))
                bar_target = {right ? hud_right - scaled_hud(bar.w, wide) : hud_left,
                    hud_bottom - scaled_hud(bar.h, wide),
                    scaled_hud(bar.w, wide), scaled_hud(bar.h, wide)};
        } else if (!has_area(bar)) {
            upper_target = {right ? hud_right - scaled_hud(upper.w, wide) : hud_left,
                hud_bottom - scaled_hud(upper.h, wide),
                scaled_hud(upper.w, wide), scaled_hud(upper.h, wide)};
        } else {
            auto layout = stack_hud(scaled_hud(upper.w, wide), scaled_hud(upper.h, wide),
                scaled_hud(bar.w, wide), scaled_hud(bar.h, wide), right,
                wide ? 400 : 256, wide ? 240 : 224);
            if (!wide) {
                layout.upper.x += 72; layout.bar.x += 72;
                layout.upper.y += 8; layout.bar.y += 8;
            }
            upper_target = layout.upper;
            bar_target = layout.bar;
        }
        // The label stays where it is when only the bar's contents moved.
        const bool redraw_upper = sprites_changed
            || !same_area(upper_target, group.upper_target);
        const bool redraw_bar = meter_changed
            || !same_area(bar_target, group.bar_target);
        if (redraw_upper) erase(group.upper_target);
        if (redraw_bar) erase(group.bar_target);
        if (redraw_upper && has_area(upper)) {
            if (!sprites_drawn) draw_sprites(element, window);
            group.upper_target = place(corner_hud_, upper,
                upper_target.x, upper_target.y);
        }
        if (redraw_bar && has_area(bar))
            group.bar_target = place(meter_hud_, bar, bar_target.x, bar_target.y);
    };
    stack(top_hud_groups_[2], render::HudElement::shield, shield_window, false,
        shield_meter_changed);
    stack(top_hud_groups_[3], render::HudElement::bombs_boost, bombs_window, true,
        bombs_meter_changed);

    if (sprites_changed) hud_sprite_cache_.capture(ppu, ppu_vram_revision);
    top_hud_object_select_ = ppu.object_select;
    top_hud_objects_enabled_ = objects_enabled;
    top_hud_meters_ = meters;
    top_hud_experience_ = experience;
    top_hud_cache_valid_ = true;

    // Groups that share pixels depend on the order they are drawn in.
    std::array<HudRect, 6U> targets{};
    std::size_t target_count = 0U;
    for (const auto& group : top_hud_groups_) {
        for (const auto* target : {&group.upper_target, &group.bar_target}) {
            const auto visible = area_inside(*target, top_hud_screen);
            if (has_area(visible) && target_count < targets.size())
                targets[target_count++] = visible;
        }
    }
    bool disjoint = true;
    for (std::size_t i = 0U; i < target_count && disjoint; ++i)
        for (std::size_t j = i + 1U; j < target_count; ++j)
            if (has_area(area_inside(targets[i], targets[j]))) {
                disjoint = false;
                break;
            }
    if (!disjoint && !rebuild) {
        // This refresh made two groups meet: draw them all again, in order.
        top_hud_cache_valid_ = false;
        return refresh_top_hud(ppu, meters, experience, ppu_memory_revision,
            ppu_vram_revision);
    }
    top_hud_disjoint_ = disjoint;

    if (top_hud_damage_all_) scan_top_hud_spans();
    else if (top_hud_damage_count_ == 0U) return false;
    else refresh_top_hud_spans();
    return true;
}

void Frame3ds::scan_top_hud_spans() {
    top_hud_spans_.clear();
    const auto& pixels = top_hud_cache_.pixels();
    scan_hud_runs(pixels.data(), 0U, static_cast<std::uint32_t>(pixels.size()),
        [this](const NativeCoverageSpan& span) { top_hud_spans_.push_back(span); });
}

// Replace the spans of the rows the last refresh rewrote. The list stays the
// one a scan of the whole layer produces.
void Frame3ds::refresh_top_hud_spans() {
    const auto& pixels = top_hud_cache_.pixels();
    const auto size = static_cast<std::uint32_t>(pixels.size());
    struct Range { std::uint32_t first, last; };
    std::array<Range, top_hud_damage_capacity> ranges{};
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < top_hud_damage_count_; ++i) {
        const auto& area = top_hud_damage_[i];
        Range range{static_cast<std::uint32_t>(area.y) * top_hud_width,
            static_cast<std::uint32_t>(area.y + area.h) * top_hud_width};
        // End on a run boundary. Pixels outside the rewritten rows did not
        // change, so no run of the previous layer crosses these ends either.
        while (range.first > 0U && pixels[range.first - 1U] != 0U) --range.first;
        while (range.last < size && pixels[range.last] != 0U) ++range.last;
        ranges[count++] = range;
    }
    std::sort(ranges.begin(), ranges.begin() + count,
        [](const Range& a, const Range& b) { return a.first < b.first; });
    std::size_t merged = 0U;
    for (std::size_t i = 1U; i < count; ++i) {
        if (ranges[i].first <= ranges[merged].last)
            ranges[merged].last = std::max(ranges[merged].last, ranges[i].last);
        else ranges[++merged] = ranges[i];
    }
    count = count == 0U ? 0U : merged + 1U;
    // Last range first: the positions of earlier spans stay valid.
    for (auto n = count; n-- > 0U;) {
        const auto range = ranges[n];
        std::array<NativeCoverageSpan, 256U> fresh;
        std::size_t fresh_count = 0U;
        bool overflow = false;
        scan_hud_runs(pixels.data(), range.first, range.last,
            [&](const NativeCoverageSpan& span) {
                if (fresh_count < fresh.size()) fresh[fresh_count++] = span;
                else overflow = true;
            });
        if (overflow) {
            scan_top_hud_spans();
            return;
        }
        const auto begin = std::partition_point(top_hud_spans_.begin(),
            top_hud_spans_.end(), [&](const NativeCoverageSpan& span) {
                return span.target_offset < range.first;
            });
        const auto end = std::partition_point(begin, top_hud_spans_.end(),
            [&](const NativeCoverageSpan& span) {
                return span.target_offset < range.last;
            });
        const auto old_count = static_cast<std::size_t>(end - begin);
        const auto shared = std::min(old_count, fresh_count);
        std::copy_n(fresh.begin(), shared, begin);
        if (fresh_count > old_count)
            top_hud_spans_.insert(begin + static_cast<std::ptrdiff_t>(shared),
                fresh.begin() + shared, fresh.begin() + fresh_count);
        else if (old_count > fresh_count)
            top_hud_spans_.erase(begin + static_cast<std::ptrdiff_t>(shared), end);
    }
}

// Mode 2's retained layer with other source sprites, first half: empty the
// spans the wide frame still shows and the rectangles the HUD refresh
// rewrote. Runs before source_foreground_spans_ is captured again.
void Frame3ds::erase_wide_source_layer(bool hud_damage) noexcept {
    auto* pixels = wide_frame_.pixels().data();
    auto* coverage = wide_frame_.mutable_write_coverage().data();
    for (const auto& span : source_foreground_spans_) {
        std::memset(pixels + span.target_offset, 0, span.count);
        std::memset(coverage + span.target_offset, 0, span.count);
    }
    if (!hud_damage) return;
    for (std::size_t i = 0U; i < top_hud_damage_count_; ++i) {
        const auto& area = top_hud_damage_[i];
        for (int y = area.y; y < area.y + area.h; ++y) {
            const auto offset = static_cast<std::size_t>(y) * top_hud_width + area.x;
            std::memset(pixels + offset, 0, static_cast<std::size_t>(area.w));
            std::memset(coverage + offset, 0, static_cast<std::size_t>(area.w));
        }
    }
}

// Second half: the newly captured spans, as
// overlay_source_coverage_on_native() writes them into a tracking frame.
void Frame3ds::draw_wide_source_layer() noexcept {
    auto* pixels = wide_frame_.pixels().data();
    auto* coverage = wide_frame_.mutable_write_coverage().data();
    const auto* source = frame_.pixels().data();
    for (const auto& span : source_foreground_spans_) {
        std::memcpy(pixels + span.target_offset, source + span.source_offset, span.count);
        std::memset(coverage + span.target_offset, 1, span.count);
    }
}

// The whole top HUD over the wide frame, coverage included. Emptying the
// previous source spans may have cut into it.
void Frame3ds::draw_wide_top_hud() noexcept {
    auto* pixels = wide_frame_.pixels().data();
    auto* coverage = wide_frame_.mutable_write_coverage().data();
    const auto* hud = top_hud_cache_.pixels().data();
    for (const auto& span : top_hud_spans_) {
        std::memcpy(pixels + span.target_offset, hud + span.source_offset, span.count);
        std::memset(coverage + span.target_offset, 1, span.count);
    }
}

// The wide frame holds the retained 2D layer of a PICA world: nothing, then
// the source foreground's written spans (Mode 2), then the top HUD. Rebuild
// exactly that inside the rectangles the HUD refresh rewrote.
void Frame3ds::patch_wide_top_hud(bool source_foreground) noexcept {
    auto* pixels = wide_frame_.pixels().data();
    auto* coverage = wide_frame_.mutable_write_coverage().data();
    const auto* source = frame_.pixels().data();
    const auto* hud = top_hud_cache_.pixels().data();
    for (std::size_t i = 0U; i < top_hud_damage_count_; ++i) {
        const auto& area = top_hud_damage_[i];
        for (int y = area.y; y < area.y + area.h; ++y) {
            const auto offset = static_cast<std::size_t>(y) * top_hud_width + area.x;
            std::memset(pixels + offset, 0, static_cast<std::size_t>(area.w));
            std::memset(coverage + offset, 0, static_cast<std::size_t>(area.w));
        }
        if (!source_foreground) continue;
        for_each_span_inside(source_foreground_spans_, area,
            [&](std::uint32_t target, std::uint32_t from, std::uint32_t count) {
                std::memcpy(pixels + target, source + from, count);
                std::memset(coverage + target, 1, count);
            });
    }
    for (std::size_t i = 0U; i < top_hud_damage_count_; ++i)
        for_each_span_inside(top_hud_spans_, top_hud_damage_[i],
            [&](std::uint32_t target, std::uint32_t from, std::uint32_t count) {
                std::memcpy(pixels + target, hud + from, count);
                std::memset(coverage + target, 1, count);
            });
}

} // namespace starfox::platform_3ds
