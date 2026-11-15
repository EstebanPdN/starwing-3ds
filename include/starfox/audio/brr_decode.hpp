#pragma once
#include <stdint.h>
#include "starfox/audio/saturate.hpp"
namespace starfox { namespace audio {
// Four BRR samples share the range and predictor. Select those once and keep
// the previous two samples in registers instead of rereading the ring buffer.
// Multiplication replaces the original negative left shift without changing
// its two's-complement result. All intermediate values fit signed 32 bits.
template<unsigned Filter, bool InvalidRange>
inline void decode_brr_group(unsigned nybbles, unsigned shift, int* pos) {
    int p1=pos[11],p2=pos[10]>>1;
    for(unsigned n=0;n<4;++n,nybbles<<=4) {
        const int nibble=static_cast<int16_t>(nybbles)>>12;
        int sample=InvalidRange?(nibble<0?-2048:0):((nibble*(1<<shift))>>1);
        if(Filter==8) sample+=p1-p2+(p2>>4)+((p1*-3)>>6);
        else if(Filter==12) sample+=p1-p2+((p1*-13)>>7)+((p2*3)>>4);
        else if(Filter==4) sample+=(p1>>1)+((-p1)>>5);
        sample=static_cast<int16_t>(saturate16(sample)*2);
        pos[n]=pos[n+12]=sample;
        p2=p1>>1;p1=sample;
    }
}
inline void decode_brr_four(unsigned nybbles, unsigned header, int* pos) {
    const unsigned shift=header>>4;
    if(shift>=13) {
        switch(header&12U) {
        case 0: decode_brr_group<0,true>(nybbles,shift,pos);break;
        case 4: decode_brr_group<4,true>(nybbles,shift,pos);break;
        case 8: decode_brr_group<8,true>(nybbles,shift,pos);break;
        case 12: decode_brr_group<12,true>(nybbles,shift,pos);break;
        }
    } else {
        switch(header&12U) {
        case 0: decode_brr_group<0,false>(nybbles,shift,pos);break;
        case 4: decode_brr_group<4,false>(nybbles,shift,pos);break;
        case 8: decode_brr_group<8,false>(nybbles,shift,pos);break;
        case 12: decode_brr_group<12,false>(nybbles,shift,pos);break;
        }
    }
}
}}
