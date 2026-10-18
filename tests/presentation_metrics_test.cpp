#include "../platform/3ds/source/presentation_metrics.hpp"
#include <cstdio>
#include <cstdlib>

using starfox::platform_3ds::PresentationMetrics;
void check(bool ok) { if (!ok) std::abort(); }
int main() {
    PresentationMetrics m;
    // An average can look fast while alternating gaps are visibly uneven.
    m.append({.session_us=1'000U});
    m.append({.session_us=2'000U, .held=0x20U});
    m.append({.session_us=34'333U});
    auto i = m.finish_interval();
    check(i.gaps==2U && i.over_25_ms==1U && i.over_50_ms==0U);
    check(i.gap_p50_ms==1.0F && i.gap_max_ms>32.3F);
    check(i.held_or==0x20U && i.moving_frames==1U);
    m.reset_epoch();
    m.append({.session_us=2'000'000U});
    i=m.finish_interval();
    check(i.gaps==0U && i.gap_max_ms==0.0F);
    for (unsigned n=1U;n<=600U;++n)
        m.append({.session_us=2'000'000U+16'667U*n});
    const auto csv=m.csv();
    unsigned lines{}; for(char c:csv) if(c=='\n') ++lines;
    check(lines==513U && csv.find("session_us,gap_us,")==0U);
    check(csv.find("3483363,16667,")!=std::string::npos);
    i=m.finish_interval();
    check(i.gaps==600U && i.over_25_ms==0U);
    check(i.gap_p95_ms>16.66F && i.gap_p95_ms<16.68F);
    std::puts("Presentation intervals: unequal gaps, reset and bounded history passed");
}
