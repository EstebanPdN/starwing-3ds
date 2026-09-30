#pragma once

#include "controls_3ds_art_data.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/render/palette.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace starfox::platform_3ds {

// Composite Esteban's hand-edited 200x107 drawing over the original SNES pad.
// Reserve otherwise-unused palette entries for its exact colours, so the
// indexed 3DS presentation keeps every outline and label pixel intact.
inline void draw_controls_3ds_art(render::Framebuffer& frame,
    render::Palette256& palette, unsigned brightness = 15U) {
    if (frame.width() != 400U || frame.height() != 240U
        || frame.draw_scale() != 1U) return;

    static const auto art=[] {
        std::array<std::uint8_t,
            controls_art_width*controls_art_height> decoded{};
        std::size_t cursor=0;
        for(const auto run:controls_art_runs) {
            const auto color=static_cast<std::uint8_t>(run>>11U);
            const auto length=static_cast<std::size_t>(run&0x07ffU);
            for(std::size_t count=0;count<length;++count)
                decoded[cursor++]=color;
        }
        return decoded;
    }();

    std::array<std::uint8_t,controls_art_colors.size()> mapped{};
    std::array<bool,256> used{};
    const auto& source=frame.pixels();
    for(const auto index:source) used[index]=true;
    // This specific cartridge backdrop is the background behind the old pad.
    // Sampling it preserves the active menu's exact brightness/palette state.
    mapped[0]=source[controls_art_y*400+controls_art_x];
    for(std::size_t color=1;color<controls_art_colors.size();++color) {
        auto want=controls_art_colors[color];
        for(auto& channel:want) channel=static_cast<std::uint8_t>(
            unsigned(channel)*std::min(brightness,15U)/15U);
        int selected=-1;
        for(unsigned index=0;index<palette.size();++index) {
            const auto& have=palette[index];
            if(have.r==want[0]&&have.g==want[1]&&have.b==want[2]) {
                selected=static_cast<int>(index);
                break;
            }
        }
        if(selected<0) for(int index=255;index>=0;--index) {
            if(used[static_cast<std::size_t>(index)]) continue;
            selected=index;
            palette[static_cast<std::size_t>(index)]={want[0],want[1],want[2],255};
            break;
        }
        if(selected>=0) {
            mapped[color]=static_cast<std::uint8_t>(selected);
            used[static_cast<std::size_t>(selected)]=true;
            continue;
        }
        // The controls scene leaves many free indices in practice. Keep a
        // bounded fallback if a future renderer ever consumes all 256.
        unsigned best=std::numeric_limits<unsigned>::max();
        for(unsigned index=0;index<palette.size();++index) {
            const auto& have=palette[index];
            const int dr=int(want[0])-int(have.r);
            const int dg=int(want[1])-int(have.g);
            const int db=int(want[2])-int(have.b);
            const unsigned distance=unsigned(dr*dr+dg*dg+db*db);
            if(distance<best) {best=distance;mapped[color]=index;}
        }
    }
    auto& target=frame.pixels();
    for(int y=0;y<controls_art_height;++y)
        for(int x=0;x<controls_art_width;++x)
            target[(controls_art_y+y)*400+controls_art_x+x]=
                mapped[art[y*controls_art_width+x]];
}

} // namespace starfox::platform_3ds
