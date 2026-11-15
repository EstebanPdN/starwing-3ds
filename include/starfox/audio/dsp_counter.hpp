#pragma once
#include <stdint.h>

namespace starfox { namespace audio {
// Exact uint32 divisibility using the odd modular inverse and a rotation.
// The table is precomputed as inverse(rate >> ctz(rate)) mod 2^32,
// UINT32_MAX / rate, ctz(rate). Keep this header usable by the C++11 SPC core.
// ARM11 has no hardware divide; neither DSP timing nor samples change.
struct DspCounterDivisor { uint32_t inverse, limit; unsigned shift; };
static constexpr DspCounterDivisor dsp_counter_divisors[32] = {
    {0x383f8801U,0x0002221dU,0}, // 30721
    {0x00000001U,0x001fffffU,11}, // 2048
    {0xaaaaaaabU,0x002aaaaaU,9}, // 1536
    {0xcccccccdU,0x00333333U,8}, // 1280
    {0x00000001U,0x003fffffU,10}, // 1024
    {0xaaaaaaabU,0x00555555U,8}, // 768
    {0xcccccccdU,0x00666666U,7}, // 640
    {0x00000001U,0x007fffffU,9}, // 512
    {0xaaaaaaabU,0x00aaaaaaU,7}, // 384
    {0xcccccccdU,0x00ccccccU,6}, // 320
    {0x00000001U,0x00ffffffU,8}, // 256
    {0xaaaaaaabU,0x01555555U,6}, // 192
    {0xcccccccdU,0x01999999U,5}, // 160
    {0x00000001U,0x01ffffffU,7}, // 128
    {0xaaaaaaabU,0x02aaaaaaU,5}, // 96
    {0xcccccccdU,0x03333333U,4}, // 80
    {0x00000001U,0x03ffffffU,6}, // 64
    {0xaaaaaaabU,0x05555555U,4}, // 48
    {0xcccccccdU,0x06666666U,3}, // 40
    {0x00000001U,0x07ffffffU,5}, // 32
    {0xaaaaaaabU,0x0aaaaaaaU,3}, // 24
    {0xcccccccdU,0x0cccccccU,2}, // 20
    {0x00000001U,0x0fffffffU,4}, // 16
    {0xaaaaaaabU,0x15555555U,2}, // 12
    {0xcccccccdU,0x19999999U,1}, // 10
    {0x00000001U,0x1fffffffU,3}, // 8
    {0xaaaaaaabU,0x2aaaaaaaU,1}, // 6
    {0xcccccccdU,0x33333333U,0}, // 5
    {0x00000001U,0x3fffffffU,2}, // 4
    {0xaaaaaaabU,0x55555555U,0}, // 3
    {0x00000001U,0x7fffffffU,1}, // 2
    {0x00000001U,0xffffffffU,0}, // 1
};
inline bool dsp_counter_fires(uint32_t counter, unsigned rate) noexcept {
    const auto& d=dsp_counter_divisors[rate];
    const uint32_t product=counter*d.inverse;
    const uint32_t rotated=(product>>d.shift)|(product<<((32U-d.shift)&31U));
    return rotated<=d.limit;
}
}}
