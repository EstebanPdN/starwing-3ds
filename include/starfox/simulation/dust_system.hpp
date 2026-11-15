#pragma once

#include "starfox/simulation/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Space dust/star field of the 3D scenes. GameSimulation owns one DustSystem,
// ticks it once per 20 Hz source update and saves it with the game state;
// render::DustRenderer only reads points().

namespace starfox::simulation {

// Points recycled per tick: the cartridge's 120, or all 511 when Star Fox EX
// sets M_MOREDOTS (see GameSimulation::dust_point_count()).
inline constexpr std::size_t kNormalDustPoints = 120U;
inline constexpr std::size_t kMaximumDustPoints = 511U;

// One point in 16-bit world coordinates; arithmetic on them wraps.
struct DustPoint {
    [[nodiscard]] bool operator==(const DustPoint&) const = default;
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
};

// Persistent native translation of MGDOTS.MC's star/dust point cloud. Point
// recycling remains on the source 20 Hz boundary; presentation only projects
// these fixed-point world positions.
class DustSystem {
public:
    DustSystem() noexcept { reset(); }

    // MINITDUST: refills every point from the fixed seed $19f8, then restores
    // that seed for the recycling that follows.
    void reset() noexcept;
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    // Throws std::runtime_error when the bytes are not a version-1 dust
    // archive; this object is unchanged then.
    void load_state(std::span<const std::uint8_t> bytes);
    // Recycles points as MSHOWDUST does, once per source update. Each of the
    // first active_count points (clamped to kMaximumDustPoints) whose offset
    // from `camera` leaves -2048 <= x,y < 2048, -2560 <= z < 2560 moves to a
    // random view-space offset (x,y in -1024..1023, z in 512..2559) rotated
    // into the world by the transpose of `world_matrix` (Q15). Does nothing
    // when `enabled` is false.
    void tick(
        const std::array<std::int16_t, 3>& camera,
        const MatrixQ15& world_matrix,
        bool enabled,
        std::size_t active_count = kNormalDustPoints) noexcept;

    [[nodiscard]] const std::array<DustPoint, kMaximumDustPoints>& points()
        const noexcept { return points_; }

private:
    [[nodiscard]] std::uint16_t next_random() noexcept;
    void recycle(
        DustPoint& point,
        const std::array<std::int16_t, 3>& camera,
        const MatrixQ15& world_matrix) noexcept;

    std::array<DustPoint, kMaximumDustPoints> points_{};
    std::uint16_t random_{0x19f8U};
    bool carry_{};
};

} // namespace starfox::simulation
