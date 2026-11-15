#pragma once

#include "native_canvas.hpp"
#include "starfox/render/background_renderer.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/simulation/snes_ppu.hpp"
#include <array>
#include <cstdint>
#include <vector>
// Copy rectangles that present Mode 1 BG2/BG3 on the 400x240 top screen from
// retained atlases. Built by frame_3ds.cpp, drawn by display_3ds.cpp; host-tested
// by tests/mode1_layer_plan_test.cpp and on console by mode1_layer_gpu_test.hpp.

namespace starfox::platform_3ds {

// A scroll changes these rectangles, not the retained pixels. Coordinates
// address the decoded, unscrolled atlases and keep SNES priority explicit.
// Each rectangle is either a 1:1 copy or a repeated edge texel/row. No
// filtering, affine approximation, or scene-dependent pixel search is used.
// SNES pixel (0,0) lands at (72,8); the 8 rows above and below the 224
// SNES lines repeat the nearest line.
class Mode1LayerPlan {
public:
    // One plane per background and SNES tile priority.
    enum Layer : unsigned { bg3_low, bg3_high, bg2_low, bg2_high, layer_count };
    // The atlas behind a layer, its revision and the extent the plan reads.
    struct Source {
        const render::Framebuffer* frame{};
        std::uint32_t revision{};
        unsigned width{}, height{};
    };
    // A screen rectangle and the atlas rectangle it copies. A source width or
    // height of 1 repeats that column or row; `solid` fills with palette index
    // `colour` instead (the tunnel wall). Index 0 is transparent.
    struct Rect {
        std::uint16_t x{}, y{}, width{}, height{};
        std::uint16_t source_x{}, source_y{}, source_width{}, source_height{};
        bool solid{};
        std::uint8_t colour{};
    };

    Mode1LayerPlan() {
        for (auto& rects : rectangles_) rects.reserve(768);
    }
    // Mode 1 without BG2/BG3 mosaic, a BG2 map of at most 512x512 pixels and
    // a BG3 map at most 512 pixels tall.
    [[nodiscard]] static bool eligible(const simulation::SnesPpuState& p) noexcept {
        const unsigned bg2_edge=p.bg2_tile_size_16?16U:8U;
        const unsigned bg2_width=((p.bg2_screen_size&1U)?64U:32U)*bg2_edge;
        const unsigned bg2_height=((p.bg2_screen_size&2U)?64U:32U)*bg2_edge;
        const unsigned bg3_height=((p.bg3_screen_size&2U)?64U:32U)*(p.bg3_tile_size_16?16U:8U);
        return p.background_mode==1U && !(p.mosaic&6U)
            && bg2_width<=512U && bg2_height<=512U && bg3_height<=512U;
    }
    // `ppu_memory_revision` is the simulation's name for this PPU's VRAM,
    // CGRAM and OAM, when the caller has it. A plan is a function of the
    // registers and tables read below, the scroll, the sources' identity
    // and extent, the tile atlas contents (named by `tiles_revision`) and,
    // for the tunnel wall, PPU memory. Between two source ticks all of them
    // repeat, and the plan of the previous frame is the plan of this one:
    // it is then kept instead of being rebuilt row by row.
    // panorama_* are the BG3 atlas planes (native_canvas.hpp) and tiles_* the
    // BG2 planes (Mode1Bg2Atlas). Returns false, leaving the plan invalid, when
    // the state is not eligible or a source has an unexpected size.
    bool build(const simulation::SnesPpuState& p, int scroll_x, int scroll_y,
        const render::Framebuffer& panorama_low, const render::Framebuffer& panorama_high,
        std::uint32_t panorama_revision, const render::Framebuffer& tiles_low,
        const render::Framebuffer& tiles_high, std::uint32_t tiles_revision,
        std::uint64_t ppu_memory_revision = 0U) {
        if (ppu_memory_revision == 0U) {
            planned_ = false;
            return plan(p,scroll_x,scroll_y,panorama_low,panorama_high,panorama_revision,
                tiles_low,tiles_high,tiles_revision);
        }
        Inputs inputs;
        inputs.memory_revision=ppu_memory_revision;
        const auto extent=[](const render::Framebuffer& f) {
            return Extent{&f,f.width(),f.height(),f.draw_scale(),f.pixels().size()};
        };
        inputs.sources={extent(panorama_low),extent(panorama_high),extent(tiles_low),extent(tiles_high)};
        inputs.panorama_revision=panorama_revision; inputs.tiles_revision=tiles_revision;
        inputs.scroll_x=scroll_x; inputs.scroll_y=scroll_y;
        inputs.background_mode=p.background_mode; inputs.mosaic=p.mosaic;
        inputs.main_screen=p.main_screen;
        inputs.bg2_screen_size=p.bg2_screen_size; inputs.bg3_screen_size=p.bg3_screen_size;
        inputs.bg2_tile_size_16=p.bg2_tile_size_16; inputs.bg3_tile_size_16=p.bg3_tile_size_16;
        inputs.bg3_high_priority=p.bg3_high_priority;
        inputs.bg3_scroll_x=p.bg3_scroll_x; inputs.bg3_scroll_y=p.bg3_scroll_y;
        inputs.bg2_scroll_x=p.bg2_scroll_x; inputs.bg2_scroll_y=p.bg2_scroll_y;
        inputs.bg2_screen_base=p.bg2_screen_base; inputs.bg2_character_base=p.bg2_character_base;
        inputs.tunnel_scene=p.tunnel_scene;
        inputs.horizontal_enabled=p.bg2_horizontal_offsets_enabled;
        inputs.scanline_enabled=p.bg2_scanline_scroll_enabled;
        // A disabled table is not read: leave it zero so it cannot differ.
        if (inputs.horizontal_enabled) inputs.horizontal=p.bg2_horizontal_offsets;
        if (inputs.scanline_enabled) inputs.scanline=p.bg2_scanline_scroll_y;
        if (planned_ && inputs==inputs_) return valid_;
        const bool result=plan(p,scroll_x,scroll_y,panorama_low,panorama_high,panorama_revision,
            tiles_low,tiles_high,tiles_revision);
        inputs_=inputs; planned_=true;
        return result;
    }
private:
    bool plan(const simulation::SnesPpuState& p, int scroll_x, int scroll_y,
        const render::Framebuffer& panorama_low, const render::Framebuffer& panorama_high,
        std::uint32_t panorama_revision, const render::Framebuffer& tiles_low,
        const render::Framebuffer& tiles_high, std::uint32_t tiles_revision) {
        for (auto& rects : rectangles_) rects.clear();
        sources_={}; valid_=false;
        if (!eligible(p)) return false;
        bg3_high_priority_=p.bg3_high_priority;
        if ((p.main_screen&4U)!=0U) {
            // Ten-bit signed HOFS and the existing panorama padding bound
            // every possible sample to columns [56,1478]. Retain 1536,
            // including both ends, even for a wider original tilemap.
            if (!plain(panorama_low) || !plain(panorama_high)
                || panorama_low.width()<1536U || panorama_low.height()<256U
                || panorama_low.height()>512U
                || (panorama_low.height()&(panorama_low.height()-1U))
                || panorama_low.width()!=panorama_high.width()
                || panorama_low.height()!=panorama_high.height()) return false;
            sources_[bg3_low]={&panorama_low,panorama_revision,1536U,panorama_low.height()};
            sources_[bg3_high]={&panorama_high,panorama_revision,1536U,panorama_high.height()};
            const int sx=int((unsigned(p.bg3_scroll_x)+512U)&1023U)-512;
            const unsigned first=native_panorama_padding-72+sx;
            for (unsigned layer : {bg3_low,bg3_high}) {
                RowBuilder rows{rectangles_[layer]};
                for (unsigned y=0;y<240U;++y) {
                    rows.begin();
                    const auto logical_y=unsigned(std::clamp(int(y)-8,0,223));
                    const unsigned sy=(logical_y+unsigned(p.bg3_scroll_y))&(panorama_low.height()-1U);
                    // Split at the 1024-pixel PICA texture limit.
                    const unsigned count=std::min(400U,1024U-(first&1023U));
                    rows.add(0,y,count,first,sy,count);
                    if (count<400U) rows.add(count,y,400U-count,first+count,sy,400U-count);
                    rows.end();
                }
            }
        }
        if ((p.main_screen&2U)!=0U) {
            const unsigned edge=p.bg2_tile_size_16?16U:8U;
            const unsigned width=((p.bg2_screen_size&1U)?64U:32U)*edge;
            const unsigned height=((p.bg2_screen_size&2U)?64U:32U)*edge;
            if (!plain(tiles_low) || !plain(tiles_high)
                || tiles_low.width()!=width || tiles_high.width()!=width
                || tiles_low.height()!=height || tiles_high.height()!=height) return false;
            sources_[bg2_low]={&tiles_low,tiles_revision,width,height};
            sources_[bg2_high]={&tiles_high,tiles_revision,width,height};
            const bool clamped=p.bg2_scanline_scroll_enabled || p.tunnel_scene;
            const auto wall=p.tunnel_scene?render::tunnel_wall_index(p):0U;
            for (unsigned layer : {bg2_low,bg2_high}) {
                RowBuilder rows{rectangles_[layer]};
                for (unsigned y=0;y<240U;++y) {
                    rows.begin();
                    const int logical_y=std::clamp(int(y)-8,0,223);
                    const int sx=p.bg2_horizontal_offsets_enabled?p.bg2_horizontal_offsets[logical_y]:scroll_x;
                    const int sy=p.bg2_scanline_scroll_enabled?p.bg2_scanline_scroll_y[logical_y]:scroll_y;
                    const unsigned source_y=unsigned(logical_y+sy)&(height-1U);
                    const int first=clamped?int(unsigned(128+sx)&(width-1U))-200:sx-72;
                    for (unsigned x=0;x<400U;) {
                        const int at=first+int(x);
                        if (clamped && (at<0 || at>=int(width))) {
                            const unsigned source_x=at<0?0U:width-1U;
                            const unsigned count=at<0?std::min(400U-x,unsigned(-at)):400U-x;
                            const auto index=source_y*width+source_x;
                            const bool fill=layer==bg2_low && !tiles_low.pixels()[index]
                                && !tiles_high.pixels()[index] && wall!=0U;
                            rows.add(x,y,count,source_x,source_y,1U,fill,std::uint8_t(wall));
                            x+=count;
                        } else {
                            const unsigned source_x=unsigned(at)&(width-1U);
                            const unsigned count=std::min(400U-x,width-source_x);
                            rows.add(x,y,count,source_x,source_y,count);
                            x+=count;
                        }
                    }
                    rows.end();
                }
            }
        }
        valid_=true; return true;
    }
public:
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    // The BG3 priority bit of the planned state.
    [[nodiscard]] bool high_priority() const noexcept { return bg3_high_priority_; }
    // BG2's high plane, and BG3's high plane when the BG3 priority bit is set,
    // are drawn over the world; the other planes below it.
    [[nodiscard]] bool above_world(unsigned layer) const noexcept {
        return layer==bg2_high || (layer==bg3_high && bg3_high_priority_);
    }
    [[nodiscard]] const Source& source(unsigned layer) const noexcept { return sources_[layer]; }
    [[nodiscard]] std::size_t storage_capacity_bytes() const noexcept {
        std::size_t bytes{};
        for(const auto& rectangles:rectangles_)bytes+=rectangles.capacity()*sizeof(Rect);
        return bytes;
    }
    [[nodiscard]] const std::vector<Rect>& rectangles(unsigned layer) const noexcept { return rectangles_[layer]; }
    [[nodiscard]] std::size_t rectangle_count() const noexcept {
        std::size_t n{}; for (const auto& r:rectangles_) n+=r.size(); return n;
    }
    // Exact CPU oracle for plan tests and diagnostic captures. The frame
    // renderer never calls this on the retained GPU presentation path.
    void replay_layer(unsigned layer, render::Framebuffer& target) const {
        if (!valid_ || target.width()!=400U || target.height()!=240U) return;
        const auto& src=sources_[layer];
        for (const auto& r:rectangles_[layer])
            for (unsigned y=0;y<r.height;++y)
                for (unsigned x=0;x<r.width;++x) {
                    const unsigned sx=r.source_x+(r.source_width==1U?0U:x);
                    const unsigned sy=r.source_y+(r.source_height==1U?0U:y);
                    const auto c=r.solid?r.colour:src.frame->pixels()[sy*src.frame->width()+sx];
                    if (c) target.set(r.x+x,r.y+y,c);
                }
    }
private:
    static bool plain(const render::Framebuffer& f) noexcept {
        return f.draw_scale()==1U && f.pixels().size()==std::size_t(f.width())*f.height();
    }
    // Adds one screen row at a time. A rectangle of the previous row grows
    // down when the new piece has the same columns and source columns and
    // either repeats its source row or continues to the next one. A row holds
    // at most four pieces.
    struct RowBuilder {
        std::vector<Rect>& out;
        std::array<std::size_t,4> previous{},current{};
        unsigned previous_count{},current_count{};
        void begin() noexcept { current_count=0U; }
        void end() noexcept { previous=current; previous_count=current_count; }
        void add(unsigned x,unsigned y,unsigned width,unsigned sx,unsigned sy,unsigned sw,
            bool solid=false,std::uint8_t colour=0U) {
            for (unsigned i=0;i<previous_count;++i) {
                auto& r=out[previous[i]];
                if (r.x!=x || r.width!=width || r.y+r.height!=y || r.source_x!=sx
                    || r.source_width!=sw || r.solid!=solid || (solid && r.colour!=colour)) continue;
                const bool repeat=(r.height==1U || r.source_height==1U) && sy==r.source_y;
                const bool advance=r.source_height==r.height && sy==r.source_y+r.source_height;
                if (!solid && !repeat && !advance) continue;
                ++r.height;
                if (!solid && advance) ++r.source_height;
                current[current_count++]=previous[i]; return;
            }
            current[current_count++]=out.size();
            out.push_back({std::uint16_t(x),std::uint16_t(y),std::uint16_t(width),1U,
                std::uint16_t(sx),std::uint16_t(sy),std::uint16_t(sw),1U,solid,colour});
        }
    };
    // Everything plan() reads, for build()'s comparison.
    struct Extent {
        const render::Framebuffer* frame{};
        std::uint32_t width{}, height{}, draw_scale{};
        std::size_t pixels{};
        friend bool operator==(const Extent&, const Extent&) = default;
    };
    struct Inputs {
        std::uint64_t memory_revision{};
        std::array<Extent,4> sources{};
        std::uint32_t panorama_revision{}, tiles_revision{};
        int scroll_x{}, scroll_y{};
        std::uint8_t background_mode{}, mosaic{}, main_screen{};
        std::uint8_t bg2_screen_size{}, bg3_screen_size{};
        bool bg2_tile_size_16{}, bg3_tile_size_16{}, bg3_high_priority{};
        bool tunnel_scene{}, horizontal_enabled{}, scanline_enabled{};
        std::int16_t bg3_scroll_x{}, bg3_scroll_y{}, bg2_scroll_x{}, bg2_scroll_y{};
        std::uint16_t bg2_screen_base{}, bg2_character_base{};
        std::array<std::int16_t,224> horizontal{}, scanline{};
        friend bool operator==(const Inputs&, const Inputs&) = default;
    };
    Inputs inputs_{};
    bool planned_{};
    std::array<Source,layer_count> sources_{};
    std::array<std::vector<Rect>,layer_count> rectangles_;
    bool valid_{},bg3_high_priority_{};
};
} // namespace starfox::platform_3ds
