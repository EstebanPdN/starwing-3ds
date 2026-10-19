#pragma once
#include <3ds.h>
#include "old_3ds_notice_art_data.hpp"

namespace starfox::platform_3ds {
template<std::size_t N>
constexpr std::size_t notice_pixels(const std::array<std::uint16_t,N>& runs) {
    std::size_t count=0;
    for (auto run:runs) count+=run&0xfffU;
    return count;
}
static_assert(notice_pixels(old_notice_top)==400U*240U);
static_assert(notice_pixels(old_notice_bottom)==320U*240U);

// Python-authored native-resolution pixels, copied without filtering to the
// rotated BGR8 framebuffer. No game assets, SD access, GPU or audio startup.
template<std::size_t N>
inline void draw_notice(gfxScreen_t screen,
    const std::array<std::uint16_t,N>& runs) {
    u16 height{},width{};
    auto* output=gfxGetFramebuffer(screen,GFX_LEFT,&height,&width);
    if (!output || height!=240U || width!=(screen==GFX_TOP?400U:320U)) return;
    std::size_t pixel=0;
    for (auto run:runs) {
        const auto color=old_notice_colors[run>>12U];
        for (unsigned i=0;i<(run&0xfffU);++i,++pixel) {
            auto* dest=output+((pixel%width)*240U+239U-pixel/width)*3U;
            dest[0]=static_cast<u8>(color>>8U);
            dest[1]=static_cast<u8>(color>>16U);
            dest[2]=static_cast<u8>(color>>24U);
        }
    }
}
inline void show_old_3ds_notice() {
    gfxSetScreenFormat(GFX_TOP,GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM,GSP_BGR8_OES);
    unsigned populated=0;
    while (aptMainLoop()) {
        hidScanInput();
        const auto pressed=hidKeysDown();
        if (pressed&(KEY_START|KEY_B)) break;
        if (pressed&KEY_TOUCH) {
            touchPosition pos{};hidTouchRead(&pos);
            if(pos.px>=70&&pos.px<250&&pos.py>=157&&pos.py<184) break;
        }
        // Populate both buffers once; this static screen has no redraw cost.
        if (populated<2U) {
            draw_notice(GFX_TOP,old_notice_top);
            draw_notice(GFX_BOTTOM,old_notice_bottom);
            gfxFlushBuffers();
            gfxSwapBuffers();
            ++populated;
        }
        gspWaitForVBlank();
    }
}
} // namespace starfox::platform_3ds
