#include "../platform/3ds/source/presentation_limit.hpp"
#include <iostream>
#include <stdexcept>
using namespace starfox::platform_3ds;
void check(bool ok) {if(!ok)throw std::runtime_error("Presentation limit violated cadence");}
int main() {
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
