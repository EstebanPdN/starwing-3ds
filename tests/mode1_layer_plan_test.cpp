#include "../platform/3ds/source/mode1_layer_plan.hpp"
#include "../platform/3ds/source/mode1_bg2_atlas.hpp"
#include <cstdio>
#include <memory>
#include <random>

using namespace starfox;
int main() {
    std::mt19937 random(0x4d310057U);
    auto p=std::make_unique<simulation::SnesPpuState>();
    auto atlas=std::make_unique<platform_3ds::Mode1Bg2Atlas>();
    auto plan=std::make_unique<platform_3ds::Mode1LayerPlan>();
    render::BackgroundRenderer renderer;
    render::Framebuffer panorama_low(1792,512),panorama_high(1792,512);
    for (unsigned i=0;i<panorama_low.pixels().size();++i) {
        auto c=std::uint8_t(random()%32U);
        (random()&1U?panorama_low:panorama_high).pixels()[i]=c;
    }
    unsigned compared{},fallbacks{};
    for (unsigned trial=0;trial<192U;++trial) {
        for (auto& b:p->vram) b=random();
        for (auto& c:p->cgram) c=random();
        p->background_mode=1;p->main_screen=(trial%13U==0U?0U:trial%5U==0U?2U:6U);
        p->bg2_screen_size=trial&3U;p->bg2_tile_size_16=(trial&4U)!=0U;
        p->bg2_character_base=random();p->bg2_screen_base=random();
        p->bg3_screen_size=3;p->bg3_tile_size_16=false;
        p->bg2_horizontal_offsets_enabled=(trial&8U)!=0U;
        p->bg2_scanline_scroll_enabled=(trial&16U)!=0U;
        p->bg3_high_priority=(trial&64U)!=0U;p->tunnel_scene=(trial&32U)!=0U;
        p->mosaic=trial%23U?0U:0x34U;
        for (auto& v:p->bg2_horizontal_offsets) v=random();
        for (auto& v:p->bg2_scanline_scroll_y) v=random();
        for (unsigned step=0;step<4U;++step) {
            p->bg3_scroll_x=std::int16_t(random());p->bg3_scroll_y=std::int16_t(random());
            const int sx=std::int16_t(random()),sy=std::int16_t(random());
            if (step==2U) p->vram[(unsigned(p->bg2_screen_base)*2U+17U)&65535U]^=0x40U;
            if (!platform_3ds::Mode1LayerPlan::eligible(*p)) { ++fallbacks; continue; }
            if (!atlas->prepare(renderer,*p) || !plan->build(*p,sx,sy,panorama_low,panorama_high,
                trial+1U,atlas->low(),atlas->high(),atlas->revision())) return 1;
            std::array<render::Framebuffer,4> reference{
                render::Framebuffer(400,240),render::Framebuffer(400,240),
                render::Framebuffer(400,240),render::Framebuffer(400,240)};
            if (p->main_screen&4U) platform_3ds::scroll_native_panorama_atlas(
                panorama_low,panorama_high,reference[0],reference[1],*p);
            renderer.draw_bg2_mode1_split(*p,sx,sy,reference[2],reference[3],72,8);
            for (unsigned layer=0;layer<4U;++layer) {
                render::Framebuffer actual(400,240);
                plan->replay_layer(layer,actual);
                if (actual.pixels()!=reference[layer].pixels()) {
                    unsigned differences{};
                    for(unsigned i=0;i<96000U;++i) differences+=actual.pixels()[i]!=reference[layer].pixels()[i];
                    std::fprintf(stderr,"FAIL trial=%u step=%u layer=%u pixels=%u\n",trial,step,layer,differences);
                    return 1;
                }
                for (const auto& r:plan->rectangles(layer)) {
                    if (!r.width || !r.height || r.x+r.width>400U || r.y+r.height>240U
                        || (r.source_width!=1U && r.source_width!=r.width)
                        || (!r.solid && r.source_height!=1U && r.source_height!=r.height)
                        || (!r.solid && (r.source_x/1024U!=(r.source_x+r.source_width-1U)/1024U
                            || r.source_x+r.source_width>plan->source(layer).width
                            || r.source_y+r.source_height>plan->source(layer).height))) return 1;
                }
                ++compared;
            }
        }
    }
    std::printf("PASS retained layers exact_planes=%u guarded_fallbacks=%u\n",compared,fallbacks);
}
