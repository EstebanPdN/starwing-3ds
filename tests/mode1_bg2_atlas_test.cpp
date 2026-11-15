#include "../platform/3ds/source/mode1_bg2_atlas.hpp"
#include <cstdio>
#include <random>
#include <stdexcept>
using namespace starfox;
int main() try {
 std::mt19937 rng(0x184ad26U);
 auto p=std::make_unique<simulation::SnesPpuState>();
 auto atlas=std::make_unique<platform_3ds::Mode1Bg2Atlas>();
 render::BackgroundRenderer renderer;
 unsigned compared{},fallbacks{};
 for(unsigned trial=0;trial<384;++trial) {
  for(auto& b:p->vram)b=rng();
  for(auto& c:p->cgram)c=rng();
  p->background_mode=1;p->main_screen=trial%13?2:0;
  p->bg2_screen_size=trial&3;p->bg2_tile_size_16=bool(trial&4);
  p->bg2_character_base=std::uint16_t(rng());p->bg2_screen_base=std::uint16_t(rng());
  p->bg2_horizontal_offsets_enabled=bool(trial&8);
  p->bg2_scanline_scroll_enabled=bool(trial&16);p->tunnel_scene=bool(trial&32);
  p->mosaic=trial%17?0U:0x32U;
  for(auto& b:p->bg2_horizontal_offsets)b=rng();
  for(auto& b:p->bg2_scanline_scroll_y)b=rng();
  const bool wide=trial%3;
  const unsigned width=wide?400:256,height=wide?240:224;
  const int origin_x=wide?72:0,origin_y=wide?8:0;
  render::Framebuffer lo(width,height),hi(width,height),ref_lo(width,height),ref_hi(width,height);
  for(unsigned step=0;step<4;++step) {
   if(step==2)p->vram[(unsigned(p->bg2_screen_base)*2U+7U)&65535U]^=0x40U;
   if(step==3) {
    const auto at=(unsigned(p->bg2_screen_base)*2U)&65535U;
    const auto tile=(p->vram[at]|unsigned(p->vram[(at+1U)&65535U])<<8U)&1023U;
    p->vram[(unsigned(p->bg2_character_base)*2U+tile*32U+3U)&65535U]^=0x5aU;
   }
   const int sx=int(std::int16_t(rng())),sy=int(std::int16_t(rng()));
   lo.clear(0xa5U);hi.clear(0x5aU);ref_lo.clear();ref_hi.clear();
   renderer.draw_bg2_mode1_split(*p,sx,sy,ref_lo,ref_hi,origin_x,origin_y);
   if(!atlas->draw(renderer,*p,sx,sy,lo,hi,origin_x,origin_y)) {++fallbacks;continue;}
   if(lo.pixels()!=ref_lo.pixels()||hi.pixels()!=ref_hi.pixels()) {
    unsigned low{},high{};for(unsigned i=0;i<lo.pixels().size();++i){low+=lo.pixels()[i]!=ref_lo.pixels()[i];high+=hi.pixels()[i]!=ref_hi.pixels()[i];}
    std::fprintf(stderr,"FAIL trial=%u step=%u low=%u high=%u\n",trial,step,low,high);return 1;
   }
   ++compared;
  }
 }
 std::printf("PASS BG2 atlas comparisons=%u guarded_fallbacks=%u retained_bytes=%zu\n",compared,fallbacks,atlas->storage_capacity_bytes());
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
