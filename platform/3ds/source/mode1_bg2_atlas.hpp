#pragma once
#include "bg2_source_cache.hpp"
#include "starfox/render/background_renderer.hpp"
#include <memory>

namespace starfox::platform_3ds {
// Retain one decoded tilemap. Scroll/HDMA only selects and copies rows;
// palette changes affect only tunnel edge fill. Bound the two cached planes
// to 512 KiB and keep larger maps/mosaic on the existing tile renderer.
class Mode1Bg2Atlas {
public:
    // `vram_revision` is the simulation's name for this PPU's VRAM, when
    // the caller has it: an unchanged source is then recognised without
    // comparing its map and characters again on every frame.
    bool prepare(render::BackgroundRenderer& renderer,const simulation::SnesPpuState& ppu,
        std::uint64_t vram_revision=0U) {
        const unsigned edge=ppu.bg2_tile_size_16?16U:8U;
        const unsigned width=((ppu.bg2_screen_size&1U)?64U:32U)*edge;
        const unsigned height=((ppu.bg2_screen_size&2U)?64U:32U)*edge;
        if(ppu.background_mode!=1U||(ppu.mosaic&2U)||width*height>512U*512U)return false;
        if(!(ppu.main_screen&2U)) return true;
        if(!source_.matches(ppu,vram_revision)) {
            auto fixed=std::make_unique<simulation::SnesPpuState>(ppu);
            fixed->bg2_horizontal_offsets_enabled=false;
            fixed->bg2_scanline_scroll_enabled=false;
            fixed->tunnel_scene=false;fixed->mosaic=0U;
            low_.resize(width,height);high_.resize(width,height);
            low_.clear();high_.clear();
            renderer.draw_bg2_mode1_split(*fixed,0,0,low_,high_);
            source_.capture(ppu,vram_revision);
            if (++revision_==0U) revision_=1U;
        }
        return true;
    }
    [[nodiscard]] const render::Framebuffer& low() const noexcept { return low_; }
    [[nodiscard]] const render::Framebuffer& high() const noexcept { return high_; }
    [[nodiscard]] std::uint32_t revision() const noexcept { return revision_; }
    bool draw(render::BackgroundRenderer& renderer,const simulation::SnesPpuState& ppu,
        int scroll_x,int scroll_y,render::Framebuffer& low,render::Framebuffer& high,
        int origin_x,int origin_y,std::uint64_t vram_revision=0U) {
        const auto plain=[](const render::Framebuffer& f) {
            return f.draw_scale()==1U&&!f.layer_tags_enabled()
                &&!f.tracks_write_coverage()&&!f.command_buffer();
        };
        const unsigned edge=ppu.bg2_tile_size_16?16U:8U;
        const unsigned width=((ppu.bg2_screen_size&1U)?64U:32U)*edge;
        const unsigned height=((ppu.bg2_screen_size&2U)?64U:32U)*edge;
        if(ppu.background_mode!=1U||(ppu.mosaic&2U)||!plain(low)||!plain(high)
            ||low.width()!=high.width()||low.height()!=high.height()
            ||width*height>512U*512U)return false;
        if(!(ppu.main_screen&2U)) {low.clear();high.clear();return true;}
        if(!prepare(renderer,ppu,vram_revision))return false;
        const bool expanded=origin_x!=0||low.width()>256U;
        const bool clamped=expanded&&(ppu.bg2_scanline_scroll_enabled||ppu.tunnel_scene);
        const auto wall=expanded&&ppu.tunnel_scene?render::tunnel_wall_index(ppu):0U;
        for(unsigned y=0;y<low.height();++y) {
            const int logical_y=origin_y?std::clamp(int(y)-origin_y,0,223):int(y);
            const auto row=unsigned(std::clamp(logical_y,0,223));
            const int sx=ppu.bg2_horizontal_offsets_enabled?ppu.bg2_horizontal_offsets[row]:scroll_x;
            const int sy=ppu.bg2_scanline_scroll_enabled?ppu.bg2_scanline_scroll_y[row]:scroll_y;
            const unsigned source_y=unsigned(logical_y+sy)&(height-1U);
            const auto* in_low=low_.pixels().data()+source_y*width;
            const auto* in_high=high_.pixels().data()+source_y*width;
            auto* out_low=low.pixels().data()+y*low.width();
            auto* out_high=high.pixels().data()+y*low.width();
            const int first=clamped?int(unsigned(128+sx)&(width-1U))-128-origin_x:sx-origin_x;
            for(unsigned x=0;x<low.width();) {
                const int at=first+int(x);
                const unsigned remaining=low.width()-x;
                if(clamped&&(at<0||at>=int(width))) {
                    const unsigned index=at<0?0U:width-1U;
                    const unsigned count=at<0?std::min(remaining,unsigned(-at)):remaining;
                    auto lo=in_low[index];const auto hi=in_high[index];
                    if(!lo&&!hi)lo=wall;
                    std::memset(out_low+x,lo,count);std::memset(out_high+x,hi,count);x+=count;
                } else {
                    const unsigned index=unsigned(at)&(width-1U);
                    const unsigned count=std::min(remaining,width-index);
                    std::memcpy(out_low+x,in_low+index,count);
                    std::memcpy(out_high+x,in_high+index,count);x+=count;
                }
            }
        }
        return true;
    }
    std::size_t storage_capacity_bytes() const noexcept {
        return low_.storage_capacity_bytes()+high_.storage_capacity_bytes();
    }
private:
    Bg2SourceCache source_;
    render::Framebuffer low_{0,0},high_{0,0};
    std::uint32_t revision_{1U};
};
}
