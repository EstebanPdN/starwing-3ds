// Independent synthetic regression adapted from upstream PR #57 (2dfe5383).
// No ROM and no GPU execution; tests current CPU output and native BG2 planner.
#include "starfox/render/background_renderer.hpp"
#include "starfox/simulation/snes_ppu.hpp"
#include "bg2_plan.hpp"
#include <iostream>
#include <memory>
#include <vector>
using namespace starfox;
void word(simulation::SnesPpuState& ppu,unsigned address,unsigned value) {
 const auto at=(address&32767U)*2U;ppu.vram[at]=value;ppu.vram[at+1]=value>>8;
}
unsigned bad_edges(const render::Framebuffer& f) {
 unsigned errors=0;
 for(unsigned y=0;y<224;++y)for(unsigned x=1;x<256;++x)
  errors+=f.get(x,y)!=f.get(x-1,y) && (x+3)%8!=0;
 return errors;
}
int main(){
 auto p=std::make_unique<simulation::SnesPpuState>();auto& ppu=*p;
 ppu.background_mode=2;ppu.main_screen=2;
 ppu.bg2_screen_base=0x4000;ppu.bg2_character_base=0x1000;
 ppu.bg2_vertical_offsets_enabled=true;ppu.bg2_horizontal_offsets_enabled=true;
 for(unsigned ink=1;ink<=3;++ink)for(unsigned y=0;y<8;++y){
  const auto row=0x2000+ink*32+y*2;
  ppu.vram[row]=(ink&1)?255:0;ppu.vram[row+1]=(ink&2)?255:0;
 }
 for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)word(ppu,0x4000+y*32+x,1+y%3);
 for(unsigned x=0;x<32;++x)word(ppu,0x2fa0+x,0x4000|(x%2?8:0));
 for(unsigned y=0;y<224;++y)ppu.bg2_horizontal_offsets[y]=3+8*(y%5);
 unsigned failures=0;
 for(int global_phase:{0,1,2,3,4,5,6,7}){
  render::Framebuffer cpu(256,224),plan(256,224);
  render::BackgroundRenderer{}.draw_bg2(ppu,global_phase,0,cpu,render::TilePriorityPass::all,0);
  std::vector<platform_3ds::Bg2Rect> rects;
  const bool ok=platform_3ds::plan_bg2_rects(ppu,global_phase,0,rects);
  if(!ok)return 2;
  for(const auto& r:rects)for(unsigned y=r.y;y<r.y+r.height;++y)for(unsigned x=r.x;x<r.x+r.width;++x)
   plan.set(x,y,r.solid?r.solid_colour:r.character);
  unsigned differences=0;for(unsigned y=0;y<224;++y)for(unsigned x=0;x<256;++x)differences+=cpu.get(x,y)!=plan.get(x,y);
  failures+=bad_edges(cpu)+bad_edges(plan)+differences;
  std::cout<<"global_phase="<<global_phase<<" hdma_phase=3 cpu_invalid_edges="<<bad_edges(cpu)
   <<" plan_invalid_edges="<<bad_edges(plan)<<" cpu_plan_pixel_differences="<<differences<<'\n';
 }
 // Independent boundary oracle: extending beyond 224 rows must select the
 // nearest HDMA row, never the unrelated global register.
 for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)word(ppu,0x4000+y*32+x,1+(x+y)%3);
 ppu.bg2_vertical_offsets_enabled=false;
 render::Framebuffer extended(400,240);
 render::BackgroundRenderer{}.draw_bg2(ppu,0,0,extended,render::TilePriorityPass::all,72,true,true,false,0,{},8);
 for(bool bottom:{false,true}) {
  auto frozen=std::make_unique<simulation::SnesPpuState>(ppu);
  frozen->bg2_horizontal_offsets.fill(ppu.bg2_horizontal_offsets[bottom?223:0]);
  render::Framebuffer oracle(400,240);
  render::BackgroundRenderer{}.draw_bg2(*frozen,0,0,oracle,render::TilePriorityPass::all,72,true,true,false,0,{},8);
  for(unsigned y=bottom?232:0;y<(bottom?240:8);++y)for(unsigned x=0;x<400;++x)
   failures+=extended.get(x,y)!=oracle.get(x,y);
 }
 ppu.bg2_vertical_offsets_enabled=true;
 for(unsigned x=0;x<32;++x)word(ppu,0x2fa0+x,0x4000|(200+x));
 for(unsigned y=0;y<224;++y)ppu.bg2_horizontal_offsets[y]=y/32;
 std::vector<platform_3ds::Bg2Rect> sparse_bands;
 if(!platform_3ds::plan_bg2_rects(ppu,0,0,sparse_bands,14560,400,240,72,8,true,true)
     || sparse_bands.size()>2000) {
  std::cerr<<"Sparse HDMA bands expanded into too many GPU rectangles: "<<sparse_bands.size()<<'\n';++failures;
 }
 std::cout<<"Sparse HDMA plan="<<sparse_bands.size()<<" rectangles\n";
 std::cout<<"HDMA top/bottom extension failures="<<failures<<'\n';
 return failures?1:0;
}
