#pragma once

#include <chrono>
#include <cstdint>

// Fixed 20 Hz simulation stepping and presentation-only interpolation. Used by
// the 3DS frame builder (transform interpolation, camera cuts, BG2 scroll) and
// by host tools and tests.
namespace starfox::timing {

// The original NTSC game completes one gameplay/strategy update for every
// three display IRQ phases. Presentation may run at 60 Hz, but gameplay must
// remain at 20 Hz so map scripts, movement, damage, and audio cues retain their
// original pace.
inline constexpr std::uint32_t kSimulationHz = 20;
inline constexpr std::uint32_t kPresentationHz = 60;

// Host speed-up keys: Tab gives 2x, Tab+Ctrl 3x and Tab+Ctrl+Shift 5x total
// speed (boosts of 100%, 200% and 400%); Shift without Ctrl adds nothing.
// Ctrl+Shift+grave (super_speed) is the separate 20x test mode and does not
// need Tab. Returns 1 when no speed-up applies.
[[nodiscard]] constexpr std::uint32_t playback_speed_multiplier(
    bool tab, bool control, bool shift, bool super_speed = false) noexcept {
    if (super_speed && control && shift) return 20U;
    if (!tab) return 1U;
    if (!control) return 2U;
    return shift ? 5U : 3U;
}

// Frame debugging must never collapse a keypress into a low-rate 20/30 FPS
// output interval (three/two cartridge rasters). Above the native raster rate,
// retain the selected interpolation cadence so every generated output frame
// remains individually inspectable.
[[nodiscard]] constexpr std::uint32_t frame_debug_presentation_hz(
    std::uint32_t selected_hz) noexcept {
    return selected_hz < kPresentationHz ? kPresentationHz : selected_hz;
}

// Whole 60 Hz raster phases to run for one presentation, and the progress
// into the next phase in [0, 1).
struct RasterPhaseBatch {
    std::uint32_t video_phases{};
    double phase_fraction{};
};

// Converts an independently selected presentation rate into the cartridge's
// fixed 60 Hz raster clock. The supported presentation rates all divide a
// common 1440 Hz timebase, so this remains exact without wall-clock drift.
// advance() throws std::invalid_argument unless presentation_hz divides 1440
// and speed_multiplier is nonzero. synchronize() sets the position within the
// current phase from a fraction clamped to [0, 1].
class RasterPhaseClock {
public:
    [[nodiscard]] RasterPhaseBatch advance(
        std::uint32_t presentation_hz,
        std::uint32_t speed_multiplier = 1U);
    void reset() noexcept;
    void synchronize(double phase_fraction) noexcept;

private:
    std::uint32_t subphase_units_{};
};

// Steps to run now, the progress toward the next step in [0, 1), and whether
// the elapsed time exceeded the maximum frame time.
struct StepBatch {
    std::uint32_t simulation_steps{};
    double interpolation_alpha{};
    bool time_was_clamped{};
};

// Converts elapsed wall-clock time into whole simulation steps with exact
// integer accounting (nanoseconds times simulation_hz), so no rounding drift
// accumulates. Negative elapsed time counts as zero; time beyond
// maximum_frame_time is dropped. The constructor throws std::invalid_argument
// for a zero or too large rate or a non-positive maximum.
class FixedStepClock {
public:
    using duration = std::chrono::nanoseconds;

    explicit FixedStepClock(
        std::uint32_t simulation_hz = kSimulationHz,
        duration maximum_frame_time = std::chrono::milliseconds{250});

    [[nodiscard]] StepBatch advance(duration elapsed);
    void reset() noexcept;

    [[nodiscard]] std::uint32_t simulation_hz() const noexcept;

private:
    std::uint32_t simulation_hz_{};
    duration maximum_frame_time_{};
    std::uint64_t phase_units_{};
};

// Measures completed host presentations independently of the requested
// presentation rate. A short sample window makes missed-frame hot spots
// visible without allowing single-frame timing noise to make the readout
// illegible. fps() changes once per sample period to the rounded average over
// that period; a timestamp before the sample start restarts the sample.
class LiveFpsCounter {
public:
    using clock = std::chrono::steady_clock;
    using duration = clock::duration;
    using time_point = clock::time_point;

    explicit LiveFpsCounter(
        duration sample_period = std::chrono::milliseconds{250});

    void reset(time_point now, std::uint32_t initial_fps = 0U) noexcept;
    void record_frame(time_point now) noexcept;
    [[nodiscard]] std::uint32_t fps() const noexcept { return fps_; }

private:
    duration sample_period_{};
    time_point sample_started_{};
    std::uint32_t sample_frames_{};
    std::uint32_t fps_{};
};

// A presentation-only transform. Fixed-point gameplay state should be copied
// into these snapshots after each 20 Hz tick. Interpolated values must never be
// written back into gameplay state. Positions are 16-bit source words held in
// int32; angles are 16-bit binary angles (65536 per turn).
struct TransformSnapshot {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    std::uint16_t pitch{};
    std::uint16_t yaw{};
    std::uint16_t roll{};
};

struct RenderTransform {
    double x{};
    double y{};
    double z{};
    double pitch{};
    double yaw{};
    double roll{};
};

// Blends previous toward current by alpha (clamped to [0, 1]) along the
// shorter 16-bit path for each position and angle. Angles in the result are
// in [0, 65536); positions are not wrapped.
[[nodiscard]] RenderTransform interpolate(
    const TransformSnapshot& previous,
    const TransformSnapshot& current,
    double alpha) noexcept;

// Scroll registers wrap at the source's address width, not necessarily at
// 16 bits. In particular CALCBGSCROLL masks BG2's vertical phase to 9 bits.
// mask + 1 is the wrap period; the shorter direction is taken and the result
// is masked.
[[nodiscard]] std::uint16_t interpolate_wrapped_scroll(
    std::uint16_t previous, std::uint16_t current, double alpha,
    std::uint16_t mask) noexcept;

// Previous-tick snapshot for an object born this tick: its current offset
// from the owner applied to the owner's previous position (16-bit wrap), with
// the owner's previous angles, so interpolation keeps it fixed relative to
// the owner.
[[nodiscard]] TransformSnapshot relative_birth_snapshot(
    const TransformSnapshot& sample, const TransformSnapshot& previous_owner,
    const TransformSnapshot& current_owner) noexcept;

// HUDROT has an enable bit and an eight-bit angle. Keep the fractional roll
// for presentation, including when the cockpit hides the player model.
// Interpolates only when both samples have the enable bit (bit 15) set;
// otherwise returns the current angle. The result is in 8-bit angle units and
// is not wrapped.
[[nodiscard]] double interpolate_cockpit_roll(std::uint16_t previous,
    std::uint16_t current, double alpha) noexcept;

// Camera scripts occasionally replace the complete fixed-point view in one
// source update. Those are cuts, not unusually fast motion, and must not be
// blended across the extra host presentations. True when any axis moves more
// than 4,096 units, compared as a signed 16-bit difference.
[[nodiscard]] bool camera_transform_is_discontinuous(
    const TransformSnapshot& previous,
    const TransformSnapshot& current) noexcept;

} // namespace starfox::timing
