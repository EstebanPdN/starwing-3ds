#include "bottom_hud_cache.hpp"
#include <cassert>
#include <memory>
#include <cstdio>
using namespace starfox;
int main() {
    using Cache=platform_3ds::BottomHudCache;
    auto ppu=std::make_unique<simulation::SnesPpuState>();
    auto cache=std::make_unique<Cache>();
    ppu->object_select=0; ppu->main_screen=0x10;
    ppu->oam.fill(0);
    // Life icon, two readout records, hidden bomb icon and moving reticle.
    ppu->oam[0]=8; ppu->oam[1]=8; ppu->oam[2]=189;
    ppu->oam[4]=16; ppu->oam[5]=8; ppu->oam[6]=226;
    ppu->oam[8]=24; ppu->oam[9]=8; ppu->oam[10]=229;
    ppu->oam[12]=248; ppu->oam[13]=248; ppu->oam[14]=188;
    ppu->oam[16]=100; ppu->oam[17]=100; ppu->oam[18]=0x61;
    simulation::MeterState meter{}; simulation::DialogueState dialogue{};
    meter.enabled=true;
    render::Palette256 palette{};
    palette[114].r=200; palette[119].r=100; palette[118].b=250;
    bool show_fps=false; const char* fps="";
    // The palette entries each part read when it was drawn.
    std::array<Cache::Colours,Cache::element_count> used{};
    Cache::use(used[Cache::lives],200); Cache::use(used[Cache::bombs],201);
    Cache::use(used[Cache::portrait],120); Cache::use(used[Cache::message],121);
    const auto sources=[&] { return Cache::sources(meter,dialogue,palette,show_fps,fps); };
    const auto stale=[&] { return cache->stale(*ppu,0,meter,dialogue,sources(),palette); };
    // Draw what is stale and check that exactly `expected` was.
    const auto redraw=[&](unsigned expected) {
        const auto found=stale();
        assert(found.elements==expected);
        assert(found.changed);
        cache->capture(found.elements,*ppu,0,meter,dialogue,sources(),palette,used);
        assert(!stale().changed&&stale().elements==0U);
    };
    const auto bit=[](Cache::Element element) { return Cache::bit(element); };
    assert(stale().elements==Cache::all_elements&&stale().sprites);
    redraw(Cache::all_elements);
    ++ppu->oam[16]; ppu->vram[0x7000]^=255;
    assert(!stale().changed); // Unrelated moving objects and background characters.
    // Status OBJ: the readout and the icons, nothing else.
    ++ppu->oam[4]; assert(stale().sprites); redraw(bit(Cache::lives)|bit(Cache::bombs));
    ppu->vram[188*32]^=255; redraw(bit(Cache::lives)|bit(Cache::bombs)); // Hidden bomb, same revision.
    ppu->main_screen=0; redraw(bit(Cache::lives)|bit(Cache::bombs));
    ppu->vram[188*32]^=255; redraw(bit(Cache::lives)|bit(Cache::bombs)); // Icons ignore screen enable.
    ppu->main_screen=0x10; redraw(bit(Cache::lives)|bit(Cache::bombs));
    // A meter moves its own fill. The picture also counts as changed when
    // the fill does not: it is presented again as it always was.
    ++meter.damage; redraw(bit(Cache::shield));
    meter.boost=12; redraw(bit(Cache::boost));
    meter.boost=40; redraw(bit(Cache::boost)); meter.boost=41;
    assert(stale().elements==0U&&stale().changed); redraw(0U); // Past the socket's 36.
    meter.shield_up=true; redraw(bit(Cache::shield)); // Its colour entry.
    meter.boss_health=9;
    assert(stale().elements==0U&&stale().changed); redraw(0U); // No boss meter below.
    // The readout samples the status layer, which carries the meters and the
    // meter-placed ENEMY label: it follows them only where they reach it.
    meter.boss_max_health=20; redraw(bit(Cache::lives));
    ppu->oam[1]=ppu->oam[5]=ppu->oam[9]=176; redraw(bit(Cache::lives)|bit(Cache::bombs));
    // The shield meter now lies under the readout: every meter change redraws it.
    meter.boost=20; redraw(bit(Cache::boost)|bit(Cache::lives));
    ++meter.damage; redraw(bit(Cache::shield)|bit(Cache::lives));
    ppu->oam[1]=ppu->oam[5]=ppu->oam[9]=8; redraw(bit(Cache::lives)|bit(Cache::bombs));
    ++meter.damage; redraw(bit(Cache::shield));
    // Communications: three parts with their own sources.
    dialogue.active=true; redraw(bit(Cache::portrait));
    ++dialogue.portrait_frame; redraw(bit(Cache::portrait));
    dialogue.text_address=0x100;
    assert(stale().elements==0U&&stale().changed); redraw(0U); // Not shown yet.
    dialogue.text_visible=true; redraw(bit(Cache::message));
    dialogue.text_address=0x200; redraw(bit(Cache::message));
    dialogue.meter_visible=true; dialogue.meter_health=20; redraw(bit(Cache::comms_meter));
    ++dialogue.meter_health; redraw(bit(Cache::comms_meter));
    dialogue.three_lines=true;
    assert(stale().elements==0U&&stale().changed); redraw(0U); // The top screen's layout.
    dialogue.active=false;
    redraw(bit(Cache::portrait)|bit(Cache::comms_meter)|bit(Cache::message));
    // The FPS panel.
    show_fps=true; fps="60.0 FPS"; redraw(bit(Cache::fps));
    fps="59.9 FPS"; redraw(bit(Cache::fps));
    show_fps=false; fps=""; redraw(bit(Cache::fps));
    // Colours: a part follows the entries it read and no others.
    ++palette[42].r; assert(!stale().changed); // Animated world colour is not drawn below.
    ++palette[200].r; redraw(bit(Cache::lives));
    ++palette[201].g; redraw(bit(Cache::bombs));
    ++palette[120].b; redraw(bit(Cache::portrait));
    ++palette[121].b; redraw(bit(Cache::message));
    ++palette[119].r; redraw(bit(Cache::shield)); // shield_up reads entry 119,
    ++palette[114].r; assert(!stale().changed);   // not 114,
    ++palette[118].g; redraw(bit(Cache::boost));  // and the boost fill 118.
    cache->invalidate();
    assert(stale().elements==Cache::all_elements&&stale().changed&&stale().sprites);
    std::puts("PASS: bottom HUD cache redraws only the part whose icons, text, palette or meter changed; ignores unrelated movement");
}
