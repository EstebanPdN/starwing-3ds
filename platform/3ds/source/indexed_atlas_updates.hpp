#pragma once
#include "starfox/render/framebuffer.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace starfox::platform_3ds {
// Exact source bytes identify changed 8x8 texture blocks. Palette changes
// dirty only blocks that actually use the changed entries. This is ordinary
// heap storage; only the final RGBA textures occupy linear GPU memory.
class IndexedAtlasUpdates {
public:
    bool update(const render::Framebuffer& source,unsigned width,unsigned height,
        bool source_changed,const std::array<bool,256>& palette_changed) {
        if (!width || !height || ((width|height)&7U) || width>source.width()
            || height>source.height() || source.draw_scale()!=1U) return false;
        const bool resized=width!=width_ || height!=height_;
        if (resized) {
            width_=width;height_=height;
            indices_.resize(std::size_t(width)*height);
            dirty_.assign(std::size_t(width/8U)*(height/8U),1U);
            counts_={};
            for(unsigned y=0;y<height;++y) {
                auto* out=indices_.data()+std::size_t(y)*width;
                std::memcpy(out,source.pixels().data()+std::size_t(y)*source.width(),width);
                for(unsigned x=0;x<width;++x)++counts_[out[x]];
            }
        } else {
            std::fill(dirty_.begin(),dirty_.end(),0U);
            if (source_changed) for(unsigned y=0;y<height;++y) {
                auto* out=indices_.data()+std::size_t(y)*width;
                const auto* in=source.pixels().data()+std::size_t(y)*source.width();
                if (std::memcmp(out,in,width)==0) continue;
                for(unsigned x=0;x<width;x+=8U) {
                    if (std::memcmp(out+x,in+x,8U)==0) continue;
                    dirty_[std::size_t(y/8U)*(width/8U)+x/8U]=1U;
                    for(unsigned i=0;i<8U;++i){--counts_[out[x+i]];++counts_[in[x+i]];}
                    std::memcpy(out+x,in+x,8U);
                }
            }
            if (std::any_of(palette_changed.begin(),palette_changed.end(),[](bool v){return v;})) {
                for(unsigned ty=0;ty<height/8U;++ty)for(unsigned tx=0;tx<width/8U;++tx) {
                    auto& dirty=dirty_[std::size_t(ty)*(width/8U)+tx];
                    if(dirty)continue;
                    for(unsigned y=0;y<8U && !dirty;++y) {
                        const auto* row=indices_.data()+std::size_t(ty*8U+y)*width+tx*8U;
                        for(unsigned x=0;x<8U;++x)if(palette_changed[row[x]]){dirty=1U;break;}
                    }
                }
            }
        }
        return true;
    }
    void mark_all() noexcept { std::fill(dirty_.begin(),dirty_.end(),1U); }
    [[nodiscard]] bool dirty(unsigned tile_x,unsigned tile_y) const noexcept {
        return dirty_[std::size_t(tile_y)*(width_/8U)+tile_x]!=0U;
    }
    [[nodiscard]] bool used(unsigned colour) const noexcept { return counts_[colour]!=0U; }
    [[nodiscard]] bool empty() const noexcept { return counts_[0]==indices_.size(); }
    [[nodiscard]] const std::uint8_t* row(unsigned y) const noexcept {return indices_.data()+std::size_t(y)*width_;}
    [[nodiscard]] std::size_t storage_capacity_bytes() const noexcept {return indices_.capacity()+dirty_.capacity();}
private:
    unsigned width_{},height_{};
    std::array<std::uint32_t,256> counts_{};
    std::vector<std::uint8_t> indices_,dirty_;
};
}
