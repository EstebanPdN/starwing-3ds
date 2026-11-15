#pragma once
#include "starfox/simulation/object_pool.hpp"
#include <cstdint>

// Sprite colour selection from source strategy flags, and the shape material
// cache key built from it. Used by the 3DS frame code and host tests.
namespace starfox::render {
// The colour-table entry an object's sprite uses: extended[21] when strategy
// flag byte 0 has bit $20 set, otherwise 0.
inline std::uint8_t source_sprite_colour(const simulation::GameObject& object) noexcept {
    return (object.strategy_flags[0] & 0x20U) ? object.extended[21] : 0U;
}
// Colour-table entries a shape decode must include for that selection (the
// entry plus one), or 0 when the object selects none.
inline std::uint16_t source_sprite_colour_count(const simulation::GameObject& object) noexcept {
    return (object.strategy_flags[0] & 0x20U)
        ? static_cast<std::uint16_t>(object.extended[21]) + 1U : 0U;
}
// Materials are immutable once cached/borrowed by a queued GPU draw. A sprite
// selecting another colour therefore needs its own variant, not an in-place
// extension which could invalidate cached texture pointers. The key packs the
// sprite colour, the shape header address and the colour table.
inline std::uint64_t source_shape_material_key(std::uint32_t header,
    std::uint16_t table, const simulation::GameObject& object) noexcept {
    return (std::uint64_t(source_sprite_colour(object)) << 48U)
        | (std::uint64_t(header) << 16U) | table;
}
}
