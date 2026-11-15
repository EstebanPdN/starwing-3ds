#include "starfox/audio/dsp_counter.hpp"
#include <cstdio>
#include <random>
int main() {
    constexpr unsigned rates[]{30721,2048,1536,1280,1024,768,640,512,384,320,256,
        192,160,128,96,80,64,48,40,32,24,20,16,12,10,8,6,5,4,3,2,1};
    std::mt19937 random(0x5f58b);
    for(unsigned rate=0;rate<32;++rate) for(unsigned i=0;i<100000;++i) {
        const std::uint32_t n=i<32768?i:random();
        if(starfox::audio::dsp_counter_fires(n,rate)!=(n%rates[rate]==0)) return 1;
    }
    std::puts("DSP counter: 3,200,000 exact divisibility checks, all rates and full 32-bit range");
}
