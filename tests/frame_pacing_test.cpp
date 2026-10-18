#include "../platform/3ds/source/frame_pacing.hpp"
#include <limits>
#include <cstdio>
#include <cstdlib>
using starfox::platform_3ds::FramePacing;
constexpr bool boundary_checks() {
    FramePacing p(60'000'000);
    if (!p.should_wait(0) || !p.should_wait(999'999) || p.should_wait(1'000'000)) return false;
    if (p.remaining_ticks(0) != 1'000'000
        || p.remaining_nanoseconds(0) != 16'666'667
        || p.remaining_ticks(999'999) != 1
        || p.remaining_nanoseconds(999'999) != 17
        || p.remaining_nanoseconds(1'000'000) != 0) return false;
    p.advance(600'000); // 10 ms left over at the sampled clock.
    if (!p.should_wait(399'999) || p.should_wait(400'000) || p.should_wait(600'000)) return false;
    p.advance(400'000);
    if (!p.ready() || p.should_wait(0) || p.remaining_nanoseconds(0) != 0) return false;
    p.consume();
    if (p.ready()) return false;
    p.advance(std::numeric_limits<std::uint64_t>::max());
    unsigned phases = 0;
    while (p.ready()) { p.consume(); ++phases; }
    if (phases != 6) return false; // Suspend catch-up is bounded to 100 ms per sample.
    p.advance(5'000'000); p.reset();
    return !p.ready() && p.should_wait(0)
        && !p.should_wait(std::numeric_limits<std::uint64_t>::max())
        && p.remaining_nanoseconds(std::numeric_limits<std::uint64_t>::max()) == 0;
}
constexpr bool rational_second() {
    constexpr std::uint64_t frequency = 268'123'480;
    FramePacing p(frequency);
    std::uint64_t previous = 0;
    for (unsigned frame = 1; frame <= 60; ++frame) {
        const auto now = (frequency * frame + 59U) / 60U;
        p.advance(now - previous); previous = now;
        if (!p.ready()) return false;
        p.consume();
        if (p.ready()) return false;
    }
    return true;
}
static_assert(boundary_checks());
static_assert(rational_second());

namespace {
constexpr std::uint64_t frequency = 268'123'480;
constexpr std::uint64_t horizon = 30U * frequency;
constexpr std::uint64_t ticks_from_us(unsigned us) {
    return (frequency * us + 500'000U) / 1'000'000U;
}
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
struct Result {
    unsigned phases{}, logic_ticks{}, audio_packets{}, presentations{}, coalesced{};
    unsigned empty_waits{}, waits_past_phase{}, delayed_wakes{};
    unsigned largest_batch{}, largest_logic_batch{}, largest_gap_us{};
};
// Deterministic timing model, not a hardware performance prediction. It uses
// the production accumulator with explicit hypothetical costs and wake delay.
Result schedule(bool deadline_sleep, unsigned vblank_hundredths,
                unsigned vblank_offset_us, unsigned base_us,
                unsigned tick_us, unsigned hud_us, bool delayed,
                unsigned phase_limit = FramePacing::max_video_phases_per_presentation,
                unsigned burst_us = 21'000U) {
    FramePacing pacing(frequency);
    Result result;
    std::uint64_t now = 0U, previous = 0U;
    unsigned iterations = 0U;
    std::uint64_t last_presentation = 0U;
    bool submitted = false;
    for (;;) {
        require(++iterations < 20'000U, "scheduler did not make progress");
        pacing.advance(now - previous);
        previous = now;
        unsigned phases = 0U, logic = 0U;
        while (pacing.ready() && phases < phase_limit) {
            pacing.consume(); ++phases; ++result.phases;
            if (result.phases % 3U == 0U) {
                ++logic; ++result.logic_ticks; ++result.audio_packets;
            }
        }
        // Drain the common final instant so both policies cover the same
        // elapsed time, without attributing a rendered frame to this boundary.
        if (now == horizon) {
            if (pacing.ready()) continue;
            break;
        }
        if (phases != 0U) {
            ++result.presentations;
            result.coalesced += phases - 1U;
            result.largest_batch = std::max(result.largest_batch, phases);
            result.largest_logic_batch = std::max(result.largest_logic_batch, logic);
            if (submitted) result.largest_gap_us = std::max(result.largest_gap_us,
                static_cast<unsigned>((now - last_presentation) * 1'000'000U / frequency));
            last_presentation = now;
            submitted = true;
        }
        auto work = ticks_from_us(phases == 0U ? 10U
            : base_us + (tick_us + hud_us) * logic);
        if (delayed && phases != 0U && result.presentations % 97U == 0U)
            work += ticks_from_us(burst_us); // Sporadic bounded work burst.
        now = std::min(horizon, now + work);
        if (now == horizon) continue;
        const auto remaining = pacing.remaining_ticks(now - previous);
        if (remaining == 0U) {
            require(pacing.remaining_nanoseconds(now - previous) == 0U,
                "ready phase would still sleep");
            continue;
        }
        std::uint64_t wake{};
        if (deadline_sleep) {
            const auto ns = pacing.remaining_nanoseconds(now - previous);
            const auto sleep_ticks = (ns * frequency + 999'999'999U) / 1'000'000'000U;
            require(sleep_ticks >= remaining && sleep_ticks <= remaining + 1U,
                "deadline conversion sleeps early or adds more than one clock tick");
            wake = now + sleep_ticks;
        } else {
            const auto offset = ticks_from_us(vblank_offset_us);
            const auto index = now < offset ? 0U
                : (now - offset) * vblank_hundredths / (frequency * 100U) + 1U;
            wake = offset + (index * frequency * 100U
                + vblank_hundredths - 1U) / vblank_hundredths;
            require(wake > now, "VBlank did not advance");
        }
        if (wake > now + remaining + 1U) ++result.waits_past_phase;
        if (phases == 0U) ++result.empty_waits;
        // Oversleep is deliberately outside the requested deadline: neither
        // an OS wake deadline nor this test promises exact physical scheduling.
        if (delayed && iterations % 31U == 0U) {
            wake += ticks_from_us(700U); ++result.delayed_wakes;
        }
        now = std::min(horizon, wake);
    }
    require(result.phases == 1800U && result.logic_ticks == 600U
        && result.audio_packets == 600U, "phase, logic or audio cadence changed");
    return result;
}
}
int main() {
    unsigned scenarios = 0U, exposed_extra_waits = 0U;
    for (unsigned hz : {6000U, 5983U})
        for (unsigned offset : {0U, 3000U, 6000U, 9000U, 12000U, 15000U})
            for (bool delayed : {false, true}) {
                const auto old = schedule(false, hz, offset, 6794U, 9500U, 4456U, delayed);
                const auto next = schedule(true, hz, offset, 6794U, 9500U, 4456U, delayed);
                require(old.phases == next.phases && old.logic_ticks == next.logic_ticks
                    && old.audio_packets == next.audio_packets, "wait policy changed cadence");
                require(next.waits_past_phase == 0U, "deadline sleep requested an avoidable extra wait");
                if (!delayed) require(next.coalesced == 0U,
                    "bounded tick burst coalesced a phase with deadline sleep");
                exposed_extra_waits += old.waits_past_phase;
                ++scenarios;
            }
    require(exposed_extra_waits != 0U, "model failed to expose the VBlank/deadline mismatch");
    const auto catch_up_old = schedule(true, 6000U, 0U,
        6500U, 14'000U, 1000U, true, 8U, 75'000U);
    const auto catch_up_next = schedule(true, 6000U, 0U,
        6500U, 14'000U, 1000U, true,
        FramePacing::max_video_phases_per_presentation, 75'000U);
    require(catch_up_old.largest_logic_batch >= 2U,
        "overdue phase fixture did not reproduce serial logic updates");
    require(catch_up_next.largest_logic_batch == 1U,
        "more than one source/audio period preceded a presentation");
    require(catch_up_next.largest_batch == 3U,
        "overdue work was not split into bounded presentations");
    require(catch_up_old.phases == catch_up_next.phases
        && catch_up_old.logic_ticks == catch_up_next.logic_ticks
        && catch_up_old.audio_packets == catch_up_next.audio_packets,
        "bounded catch-up changed the source or audio clock");
    std::printf("Catch-up: batches %u -> %u, maximum source ticks per image %u -> %u; both preserved 1800 phases, 600 ticks and 600 audio packets.\n",
        catch_up_old.largest_batch, catch_up_next.largest_batch,
        catch_up_old.largest_logic_batch, catch_up_next.largest_logic_batch);
    std::printf("Pacing: %u timing scenarios preserved 1800 phases, 600 logic ticks and 600 audio packets each; deadline sleep adds no requested wait past the due phase.\n", scenarios);
}
