#pragma once
#include <cstdint>
#include <string_view>
namespace starfox::platform_3ds {
#ifdef STARWING_BUILD_VERSION
inline constexpr std::string_view test_build_label=STARWING_BUILD_VERSION;
#else
inline constexpr std::string_view test_build_label="Prueba";
#endif
static_assert(test_build_label.size()<=25U);
// The bottom-screen pixels draw_test_build_label() writes: left, top, right
// and bottom, half open; no pixels when the badge is not drawn. A partial
// conversion of the bottom image that reaches them must draw the badge again.
struct TestBuildLabelArea { int left, top, right, bottom; };
inline constexpr TestBuildLabelArea test_build_label_area=
    test_build_label.starts_with("Prueba")
    ? TestBuildLabelArea{(320-static_cast<int>(test_build_label.size())*12-8)/2,222,
        (320-static_cast<int>(test_build_label.size())*12-8)/2
            +static_cast<int>(test_build_label.size())*12+8,240}
    : TestBuildLabelArea{0,0,0,0};
// Persistent diagnostic badge, in the native rotated bottom framebuffer.
inline void draw_test_build_label(std::uint32_t* framebuffer) {
    if(!test_build_label.starts_with("Prueba"))return;
    constexpr unsigned char letters[7][7] = {
        {30,17,17,30,16,16,16}, // P
        {0,0,22,25,16,16,16}, // r
        {0,0,17,17,17,19,13}, // u
        {0,0,14,17,31,16,14}, // e
        {16,16,22,25,17,17,30}, // b
        {0,0,14,1,15,17,15}, // a
        {0,0,0,0,0,0,0}
    };
    constexpr unsigned char digits[10][7] = {
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
    };
    const auto length=static_cast<int>(test_build_label.size());
    const auto left=(320-length*12-8)/2;
    for (int x=left;x<left+length*12+8;++x) for(int y=222;y<240;++y)
        framebuffer[x*240+239-y]=0x131b2dffU;
    for(int c=0;c<length;++c) for(int y=0;y<7;++y) for(int x=0;x<5;++x) {
        const auto character=test_build_label[c];
        const auto row=c<6?letters[c][y]
            :character>='0' && character<='9'?digits[character-'0'][y]:0U;
        if(row & (1U<<(4-x)))
            for(int dx=0;dx<2;++dx) for(int dy=0;dy<2;++dy)
                framebuffer[(left+5+c*12+x*2+dx)*240+239-(224+y*2+dy)]=0xffffffffU;
    }
}
}
