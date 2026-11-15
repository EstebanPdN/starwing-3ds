#include "../platform/3ds/source/presentation_limit.hpp"
#include "../platform/3ds/source/hardware_profile.hpp"
#include <iostream>
#include <stdexcept>
using namespace starfox::platform_3ds;
void check(bool ok) {if(!ok)throw std::runtime_error("Presentation limit violated cadence");}
int main() {
    check(hardware_profile(false).default_fps_limit==30U);
    check(hardware_profile(true).default_fps_limit==60U);
    for(unsigned legacy:{0U,30U,60U,99U}) {
        check(profile_fps_limit(30U,legacy,0U)==30U);
        check(profile_fps_limit(60U,legacy,1U)==60U);
    }
    for(unsigned saved:{0U,30U,60U})check(profile_fps_limit(30U,saved,fps_profile_policy)==saved);
    check(profile_fps_limit(30U,99U,fps_profile_policy)==30U);
    for(unsigned rate:{0U,30U,60U}) {
        PhasePresentationLimit cadence;unsigned count=0;
        check(!cadence.accept(0,rate));
        for(unsigned phase=0;phase<3600;++phase) count+=cadence.accept(1,rate);
        check(count==(rate==30U?1800U:3600U));
    }
    PhasePresentationLimit phases;
    check(phases.accept(1,30));check(!phases.accept(1,30));check(phases.accept(1,30));
    check(phases.accept(3,30));check(phases.accept(1,30));check(!phases.accept(0,30));
    check(phases.accept(1,60));check(phases.accept(1,30));check(!phases.accept(1,30));
    constexpr unsigned long long hz=268123480;
    for(unsigned limit:{0U,30U,60U}) {
        PresentationLimit gate(hz);unsigned frames{};
        for(unsigned phase=0;phase<3600;++phase)
            frames+=gate.accept((hz*phase+59)/60,limit);
        check(frames==(limit==30?1800U:3600U));
    }
    PresentationLimit gate(hz);
    check(gate.accept(0,30));check(!gate.accept(hz/100,30));
    check(gate.accept(hz*10,30));check(!gate.accept(hz*10,30));check(!gate.accept(hz*10+1,30));
    check(gate.accept(hz*10,60));check(!gate.accept(hz*10,60));
    check(gate.accept(hz*10,0));check(gate.accept(hz*10,0));
    check(valid_fps_limit(31)==0&&valid_fps_limit(60)==60);
    std::cout<<"PASS: OFF/30/60 cadence, late frames, rate changes and validation\n";
}
