#include "starfox/audio/saturate.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
int main() {
    const auto check=[](int value) {
        const int expected=value < -32768 ? -32768 : value > 32767 ? 32767 : value;
        if(starfox::audio::saturate16(value)!=expected) throw std::runtime_error("Saturation changed a sample");
    };
    for(int value=-65536;value<=65536;++value) check(value);
    check(std::numeric_limits<int>::min());check(std::numeric_limits<int>::max());
    std::mt19937 random(0x5f58b);
    for(unsigned i=0;i<100000;++i)check(static_cast<std::int32_t>(random()));
    std::cout<<"PASS: 231075 saturation values, including both 32-bit limits\n";
}
