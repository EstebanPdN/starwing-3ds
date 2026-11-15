#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/simulation/object_pool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Particle pool of the Super FX object drawing. GameSimulation ticks it once
// per source frame and saves it with the game state; render::ParticleRenderer
// draws particles() relative to each owner object.

namespace starfox::simulation {

inline constexpr std::size_t kMaximumParticles = 300;

// A free slot has life 0. x/y/z are the offset from the owner object and
// previous_* the offset before the last move.
struct ParticleState {
    [[nodiscard]] bool operator==(const ParticleState&) const = default;
    std::uint8_t life{};
    std::uint8_t flags{};
    std::uint8_t colour{};
    std::int8_t velocity_x{};
    std::int8_t velocity_y{};
    std::int8_t velocity_z{};
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
    std::int16_t previous_x{};
    std::int16_t previous_y{};
    std::int16_t previous_z{};
    ObjectHandle owner{};
};

// Native equivalent of MPART.MC's persistent 300-entry Super FX pool.
// Keeps a pointer to `rom`, which must outlive it.
class ParticleSystem {
public:
    // Reads PARTFADETAB and PARTICLE_CIRCLE; throws std::runtime_error when
    // either symbol is missing.
    ParticleSystem(
        const assets::RomImage& rom,
        const assets::SymbolMap& symbols);
    ParticleSystem(
        const assets::RomImage& rom,
        std::uint32_t fade_table,
        std::uint32_t circle_table) noexcept;

    // Frees every slot and reseeds the generator with $1234.
    void reset() noexcept;
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    // Throws std::runtime_error for another version, other table addresses,
    // or an owner handle above kMaximumObjects; this object is unchanged then.
    void load_state(std::span<const std::uint8_t> bytes);
    // MDRAWLIS.MC's particle pass over the active objects with strategy flag
    // bit 4 set and the invisible bit (27) clear: one with a particle type in
    // scratch_bytes[2] emits scratch_bytes[0] particles of life
    // scratch_bytes[1], then its particles move. Every particle then ages; one
    // whose owner was skipped is freed. Does nothing when `enabled` is false.
    void tick(const ObjectPool& objects, bool enabled);

    [[nodiscard]] const std::array<ParticleState, kMaximumParticles>& particles()
        const noexcept { return particles_; }
    [[nodiscard]] std::size_t active_count() const noexcept;

private:
    void make_particles(
        ObjectHandle owner,
        std::uint8_t type,
        std::uint8_t life,
        std::uint8_t count);
    void show_particles(ObjectHandle owner);
    void update_particles();
    [[nodiscard]] std::uint16_t next_random() noexcept;
    [[nodiscard]] std::int8_t circle(std::size_t index) const;

    const assets::RomImage* rom_{};
    std::uint32_t fade_table_{};
    std::uint32_t circle_table_{};
    std::array<ParticleState, kMaximumParticles> particles_{};
    std::uint16_t random_{0x1234U};
};

} // namespace starfox::simulation
