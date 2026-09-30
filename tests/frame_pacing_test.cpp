#include "../platform/3ds/source/frame_pacing.hpp"
#include <limits>
using starfox::platform_3ds::FramePacing;
constexpr bool boundary_checks() {
    FramePacing p(60'000'000);
    if (!p.should_wait(0) || !p.should_wait(999'999) || p.should_wait(1'000'000)) return false;
    p.advance(600'000); // 10 ms left over at the sampled clock.
    if (!p.should_wait(399'999) || p.should_wait(400'000) || p.should_wait(600'000)) return false;
    p.advance(400'000);
    if (!p.ready() || p.should_wait(0)) return false;
    p.consume();
    if (p.ready()) return false;
    p.advance(std::numeric_limits<std::uint64_t>::max());
    unsigned phases = 0;
    while (p.ready()) { p.consume(); ++phases; }
    if (phases != 6) return false; // Suspend catch-up is bounded to 100 ms per sample.
    p.advance(5'000'000); p.reset();
    return !p.ready() && p.should_wait(0)
        && !p.should_wait(std::numeric_limits<std::uint64_t>::max());
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
int main() {}
