#pragma once
#include <cstdint>

// Presentation effect IDs offered by the pregame MODEL EFFECTS and WORLD
// EFFECTS options; the chosen IDs are part of the simulation's saved host state.
namespace starfox::render {
// Append styles so existing saved effect IDs retain their meaning.
enum class Effect : std::uint8_t {
    off, cel_drawn, ink, neon, monochrome, dithered, blueprint, bloom,
    sepia, thermal, night_vision, pastel, comic, vaporwave, count
};
inline constexpr auto effect_count = static_cast<std::uint8_t>(Effect::count);
// Whether `value` is offered by MODEL EFFECTS (world false) or WORLD EFFECTS
// (world true). Neither offers bloom; MODEL EFFECTS omits blueprint and
// WORLD EFFECTS omits cel_drawn.
inline constexpr bool selectable_effect(std::uint8_t value, bool world) {
    return value < effect_count && value != static_cast<std::uint8_t>(Effect::bloom)
        && value != static_cast<std::uint8_t>(world ? Effect::cel_drawn : Effect::blueprint);
}
// The next (or, backwards, the previous) selectable ID, wrapping around. `off`
// is always selectable, so the search ends.
inline constexpr std::uint8_t next_effect(std::uint8_t value, bool world, bool backwards) {
    do { value = (value + (backwards ? effect_count - 1U : 1U)) % effect_count; }
    while (!selectable_effect(value, world));
    return value;
}
}
