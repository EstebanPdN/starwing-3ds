#include "starfox/vr/source_sprites.hpp"
#include "starfox/vr/packed_vram.hpp"
#include "starfox/vr/background_tiles.hpp"
#include "starfox/render/palette.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include "starfox/render/sprite_renderer.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace starfox::vr {
std::vector<DrawPacket> source_dialogue_packets(const assets::RomImage& rom,
    const assets::SymbolMap& symbols,const simulation::DialogueState& dialogue,
    render::ScaledTextRenderer& layout,const std::array<uint16_t,256>& cgram,
    unsigned brightness,bool srgb) {
    std::vector<DrawPacket> packets;
    if(!dialogue.active) return packets;
    packets.push_back(source_portrait_packet(rom,symbols,dialogue.portrait_frame,
        dialogue.alternate_portraits,cgram,brightness,srgb));
    if(dialogue.text_visible && (dialogue.text_address&0xffffU)>=0x8000U) {
        const auto translated=layout.translated_game_text_lines(dialogue.text_address,92);
        int y=dialogue.three_lines?153:169;
        if(!translated.empty()) y=std::min(y,183-10*(int(translated.size())-1));
        for(unsigned pass=0;pass<2;++pass) {
            const auto ink=pass?uint8_t(112+(rom.read8(dialogue.text_address)&15U)):uint8_t(121);
            if(!translated.empty()) {
                int line_y=y+(pass?0:1);
                for(const auto line:translated) {
                    packets.push_back(source_unicode_ui_text_packet(rom,symbols,line,
                        pass?82:83,line_y,ink,cgram,brightness,srgb));
                    line_y+=10;
                }
            } else {
                packets.push_back(source_game_text_packet(rom,symbols,dialogue.text_address,
                    pass?82:83,y+(pass?0:1),174+(pass?0:1),256,ink,cgram,brightness,srgb));
            }
        }
    }
    packets.push_back(source_dialogue_meter_packet(dialogue,cgram,brightness,srgb));
    return packets;
}
DrawPacket source_dialogue_meter_packet(const simulation::DialogueState& dialogue,
    const std::array<uint16_t,256>& cgram,unsigned brightness,bool srgb) {
    if(brightness>15) throw std::invalid_argument("Invalid dialogue meter brightness");
    DrawPacket packet;
    if(!dialogue.active || !dialogue.meter_visible) return packet;
    const auto rectangle=[&](int left,int top,int right,int bottom,unsigned index) {
        if(right<=left) return;
        const auto colour=source_backdrop_colour(cgram[index],brightness,srgb);
        const int corners[4][2]{{left,top},{right,top},{right,bottom},{left,bottom}};
        for(unsigned corner:{0U,1U,2U,0U,2U,3U}) {
            SceneVertex v{};v.position[0]=float(corners[corner][0]);v.position[1]=float(corners[corner][1]);
            std::copy(colour.begin(),colour.end(),v.color);packet.geometry.vertices.push_back(v);
        }
    };
    rectangle(82,177,126,178,126);rectangle(82,188,126,189,126);
    rectangle(82,178,83,188,126);rectangle(125,178,126,188,126);
    // The source frame wins over the fill at x=125, even for corrupt health.
    rectangle(84,179,84+std::min<unsigned>(dialogue.meter_health,41),187,114);
    return packet;
}
DrawPacket source_portrait_packet(const assets::RomImage& rom,const assets::SymbolMap& symbols,
    uint8_t frame,bool alternate,const std::array<uint16_t,256>& cgram,unsigned brightness,bool srgb) {
    const auto address=[&](const char* name) {
        for(auto value:symbols.find(name))
            if((value&0xffffU)>=0x8000U && ((value>>16)&255U)<0x70U) return value;
        return uint32_t{};
    };
    auto base=alternate?address("FACEDATA2"):0U;
    if(!base) base=address("FACEDATA");
    if(!base) throw std::runtime_error("Missing portrait data");
    // Keep the authored planar tiles packed: the existing background shader
    // decodes their four bitplanes and palette, with no CPU pixel rasterization.
    simulation::SnesPpuState ppu{};
    ppu.main_screen=1;ppu.background_mode=1;ppu.bg1_screen_base=512;
    ppu.cgram=cgram;
    for(unsigned i=0;i<640;++i) ppu.vram[i]=rom.read8(base+uint32_t(frame)*640+i);
    for(unsigned x=0;x<4;++x) for(unsigned y=0;y<5;++y) {
        const unsigned offset=1024+(y*32+x)*2;
        const unsigned tile=x*5+y;
        ppu.vram[offset]=uint8_t(tile);ppu.vram[offset+1]=28; // Palette seven.
    }
    BackgroundTileOptions options;options.brightness=brightness;
    auto packet=background_tile_packet(ppu,BackgroundLayer::bg1,options,srgb);
    packet.geometry.vertices.clear();
    for(bool ink:{false,true}) for(unsigned corner:{0U,1U,2U,0U,2U,3U}) {
        const float u=(corner==1 || corner==2)?32.F:0.F,v=corner>=2?40.F:0.F;
        SceneVertex vertex{};
        // Correct SNES pixel aspect without moving the right edge into text.
        vertex.position[0]=80.F-(32.F-u)*7.F/6.F;vertex.position[1]=152.F+v;
        vertex.uv[0]=u;vertex.uv[1]=v;
        // Tile index zero is transparent in the shared shader. Its native
        // portrait meaning is palette 112, supplied by this backing quad.
        vertex.texture[1]=ink?0U:112U;vertex.texture[3]=(ink?8U:32U)|(srgb?2U:0U);
        packet.geometry.vertices.push_back(vertex);
    }
    return packet;
}
DrawPacket source_shutter_packet(const simulation::WindowWipeState& previous,
    const simulation::WindowWipeState& current,double alpha) {
    if(!std::isfinite(alpha)) throw std::invalid_argument("Invalid shutter interpolation");
    DrawPacket packet;
    const auto wipe=simulation::interpolate_window_wipe(previous.active?previous:current,current,alpha);
    if(!wipe.active || !wipe.horizontal_opening) return packet;
    const auto rectangle=[&](float top,float bottom) {
        if(bottom<=top) return;
        const float corners[4][2]{{-1,top},{1,top},{1,bottom},{-1,bottom}};
        for(unsigned corner:{0U,1U,2U,0U,2U,3U}) {
            SceneVertex vertex{};
            vertex.position[0]=corners[corner][0];vertex.position[1]=corners[corner][1];
            vertex.color[3]=1;vertex.odd_color[3]=1;
            packet.geometry.vertices.push_back(vertex);
        }
    };
    rectangle(-1.F,float(-1.+2.*std::clamp(wipe.opening_top,0.,192.)/192.));
    rectangle(float(-1.+2.*std::clamp(wipe.opening_bottom,0.,192.)/192.),1.F);
    return packet;
}
DrawPacket source_circle_packet(const simulation::CircleEffectState& previous,
    const simulation::CircleEffectState& current,double alpha,unsigned brightness,bool srgb) {
    if(!std::isfinite(alpha) || brightness>15) throw std::invalid_argument("Invalid circle presentation");
    DrawPacket packet;
    if(!current.active || !(current.affected_layers&0x3f)) return packet;
    alpha=std::clamp(alpha,0.,1.);
    const auto& from=previous.active?previous:current;
    const float radius=float(std::lerp(previous.active?double(previous.radius):0.,double(current.radius),alpha));
    if(radius<=0) return packet;
    const auto center=[alpha](int16_t a,int16_t b) {
        int delta=int(b)-int(a);if(delta>32767) delta-=65536;else if(delta< -32768) delta+=65536;
        return float(a+delta*alpha);
    };
    const float x=center(from.centre_x,current.centre_x),y=center(from.centre_y,current.centre_y);
    const auto channel=[alpha](uint8_t a,uint8_t b) {return uint16_t(std::lround(std::lerp(double(a&31),double(b&31),alpha)));};
    auto color=source_backdrop_colour(channel(from.red,current.red)|(channel(from.green,current.green)<<5)
        |(channel(from.blue,current.blue)<<10),brightness,srgb);
    color[3]=(current.affected_layers&0x40)? .5F:1.F;
    constexpr float corners[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
    for(unsigned i:{0U,1U,2U,0U,2U,3U}) {
        SceneVertex v{};v.position[0]=x+corners[i][0]*radius;v.position[1]=y+corners[i][1]*radius;
        v.uv[0]=corners[i][0];v.uv[1]=corners[i][1];v.texture[3]=4096;
        std::copy(color.begin(),color.end(),v.color);packet.geometry.vertices.push_back(v);
    }
    return packet;
}
std::vector<DrawPacket> source_mode3_packets(const simulation::SnesPpuState& ppu,
    unsigned brightness,bool srgb,std::optional<std::array<int16_t,2>> bg2_scroll) {
    if(ppu.background_mode!=3) throw std::invalid_argument("Mode-3 compositor requires Mode 3");
    std::vector<DrawPacket> packets;
    for(unsigned priority:{1U,2U}) {
        BackgroundTileOptions options;options.brightness=brightness;options.priority=priority;
        options.scroll_override=bg2_scroll;
        packets.push_back(background_tile_packet(ppu,BackgroundLayer::bg2,options,srgb));
        packets.push_back(source_sprite_packet(ppu,brightness,(priority-1)*2,srgb));
        options.scroll_override.reset();
        packets.push_back(background_tile_packet(ppu,BackgroundLayer::bg1,options,srgb));
        packets.push_back(source_sprite_packet(ppu,brightness,(priority-1)*2+1,srgb));
    }
    return packets;
}
DrawPacket source_game_text_packet(const assets::RomImage& rom,const assets::SymbolMap& symbols,
    uint32_t address,int x,int y,int right_clip,size_t characters,uint8_t ink,
    const std::array<uint16_t,256>& cgram,unsigned brightness,bool srgb) {
    if(brightness>15) throw std::invalid_argument("Invalid text brightness");
    if((address&0xffffU)<0x8000U || !characters) return {};
    std::string text;
    for(size_t i=0;i<std::min<size_t>(characters,256);++i) {
        auto value=rom.read8(address+1+uint32_t(i));if(!value) break;text.push_back(char(value));
    }
    return source_ui_text_packet(rom,symbols,text,x,y,right_clip,ink,cgram,brightness,srgb);
}
DrawPacket source_unicode_ui_text_packet(const assets::RomImage& rom,const assets::SymbolMap& symbols,
    std::u32string_view text,int x,int y,uint8_t ink,const std::array<uint16_t,256>& cgram,unsigned brightness,bool srgb) {
    if(brightness>15 || ink==0) throw std::invalid_argument("Invalid Unicode menu ink/brightness");
    render::Framebuffer bitmap(256,224);
    render::ScaledTextRenderer font(rom,symbols);
    font.draw_unicode(text.substr(0,256),x,y,bitmap,ink,0);
    DrawPacket packet;
    const auto colour=render::decode_bgr555_palette(cgram)[ink];
    const uint32_t rgba=uint32_t(colour.r*brightness/15)|(uint32_t(colour.g*brightness/15)<<8)
        |(uint32_t(colour.b*brightness/15)<<16)|0xff000000U;
    for(unsigned top=0;top<224;top+=16) for(unsigned left=0;left<256;left+=16) {
        std::array<uint32_t,8> rows{};bool visible=false;
        for(unsigned row=0;row<16;++row) for(unsigned column=0;column<16;++column)
            if(bitmap.pixels()[(top+row)*256+left+column]!=0) {
                rows[row/2]|=(0x8000U>>column)<<((row&1)*16);visible=true;
            }
        if(!visible) continue;
        const auto offset=uint32_t(packet.geometry.texels.size());packet.geometry.texels.push_back(rgba);
        packet.geometry.texels.insert(packet.geometry.texels.end(),rows.begin(),rows.end());
        for(unsigned corner:{0U,1U,2U,0U,2U,3U}) {
            SceneVertex v{};const float dx=(corner==1 || corner==2)?16.f:0.f,dy=corner>=2?16.f:0.f;
            v.position[0]=float(left)+dx;v.position[1]=float(top)+dy;v.uv[0]=dx;v.uv[1]=dy;
            v.texture[0]=offset;v.texture[1]=v.texture[2]=15;v.texture[3]=1024U|(srgb?2U:0U);
            packet.geometry.vertices.push_back(v);
        }
    }
    return packet;
}
DrawPacket source_ui_text_packet(const assets::RomImage& rom,const assets::SymbolMap& symbols,
    std::string_view text,int x,int y,int right_clip,uint8_t ink,
    const std::array<uint16_t,256>& cgram,unsigned brightness,bool srgb) {
    DrawPacket packet;
    if(brightness>15) throw std::invalid_argument("Invalid text brightness");
    text=text.substr(0,256);
    const auto symbol=[&](const char* name) {
        for(auto value:symbols.find(name))
            if((value&0xffffU)>=0x8000U && ((value>>16)&255)<0x70) return value;
        throw std::runtime_error(std::string("Missing text font: ")+name);
    };
    const auto widths=symbol("FONT0WID"),font=symbol("FONT0FON"),translation=symbol("FONT0TRN");
    const auto width=[&](uint8_t c)->unsigned {
        if(c==':' || c=='/' || c=='>') return 5;
        return c==32?5:c<32?0:rom.read8(widths+rom.read8(translation+c-32));
    };
    const auto colour=render::decode_bgr555_palette(cgram)[ink];
    const uint32_t rgba=uint32_t(colour.r*brightness/15)
        |(uint32_t(colour.g*brightness/15)<<8)|(uint32_t(colour.b*brightness/15)<<16)|0xff000000U;
    size_t start=0;
    while(start<text.size() && y<224) {
        auto end=text.size(),next=text.size(),space=text.size();int total=0;
        for(size_t i=start;i<text.size();++i) {
            const int w=int(width(text[i]));if(text[i]==32) space=i;
            if(x+total+w>right_clip) {
                if(space!=text.size() && space>=start) {end=space;next=space+1;}
                else {end=i;next=i;}break;
            }
            total+=w;
        }
        int cursor=x;
        for(size_t i=start;i<end;++i) {
            const auto c=text[i];const unsigned w=width(c);
            if(c>32 && w) {
                if(w>16) throw std::runtime_error("Invalid source glyph width");
                const auto glyph=font+uint32_t(rom.read8(translation+c-32))*24;
                const auto offset=uint32_t(packet.geometry.texels.size());
                packet.geometry.texels.push_back(rgba);
                // The cartridge table aliases host punctuation to unrelated
                // glyphs. Supply actual menu punctuation without changing the
                // authored dialogue/font translation tables.
                const auto glyph_row=[&](unsigned row)->uint32_t {
                    if(row>=12) return 0;
                    if(c==':') return row==3 || row==4 || row==8 || row==9?0x6000U:0U;
                    if(c=='/') return 0x8000U>>(3-row*4/12);
                    if(c=='>') return row>=2 && row<=9?0x8000U>>(row<=5?row-2:9-row):0U;
                    return rom.read16(glyph+row*2);
                };
                for(unsigned row=0;row<16;row+=2)
                    packet.geometry.texels.push_back(glyph_row(row)|(glyph_row(row+1)<<16));
                for(unsigned corner:{0U,1U,2U,0U,2U,3U}) {
                    SceneVertex v{};const float dx=(corner==1 || corner==2)?float(w):0;
                    const float dy=corner>=2?12.F:0;
                    v.position[0]=float(cursor)+dx;v.position[1]=float(y)+dy;
                    v.uv[0]=dx;v.uv[1]=dy;v.texture[0]=offset;
                    v.texture[1]=v.texture[2]=15;v.texture[3]=1024U|(srgb?2U:0U);
                    packet.geometry.vertices.push_back(v);
                }
            }
            cursor+=int(w);
        }
        if(next==text.size()) break;
        start=next<=start?start+1:next;y+=13;
    }
    return packet;
}
DrawPacket source_meter_packet(const simulation::MeterState& meters,
    const std::array<uint16_t,256>& cgram,unsigned brightness,bool srgb,
    unsigned viewport_width,bool anchor_to_edges,const render::HudLayout* layout) {
    if(brightness>15 || viewport_width==0 || viewport_width>8192)
        throw std::invalid_argument("Invalid GPU meter options");
    DrawPacket packet;packet.model={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const auto rectangles=render::meter_rectangles(meters,viewport_width,anchor_to_edges,layout);
    for(size_t i=0;i<rectangles.count;++i) {
        const auto& r=rectangles.rectangles[i];
        const int left=std::max(r.x,0),right=std::min(r.x+r.width,int(viewport_width));
        const int top=std::max(r.y,0),bottom=std::min(r.y+r.height,224);
        if(right<=left || bottom<=top) continue;
        const int corners[4][2]{{left,top},{right,top},{right,bottom},{left,bottom}};
        for(unsigned corner:{0U,1U,2U,0U,2U,3U}) {
            SceneVertex v{};v.position[0]=float(corners[corner][0]);v.position[1]=float(corners[corner][1]);
            v.texture[1]=r.colour;v.texture[3]=32|(srgb?2:0);packet.geometry.vertices.push_back(v);
        }
    }
    if(packet.geometry.vertices.empty()) return packet;
    auto& data=packet.geometry.texels;data.resize(272);data[13]=15-brightness;
    const auto palette=render::decode_bgr555_palette(cgram);
    for(unsigned i=0;i<256;++i) data[16+i]=palette[i].r|(uint32_t(palette[i].g)<<8)|(uint32_t(palette[i].b)<<16)|0xff000000U;
    return packet;
}
DrawPacket source_sprite_packet(const simulation::SnesPpuState& ppu,unsigned brightness,
    std::optional<unsigned> priority,bool srgb,const simulation::MeterState* meters) {
    if(brightness>15 || (priority && *priority>3)) throw std::invalid_argument("Invalid source sprite options");
    DrawPacket packet;packet.model={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    if(!(ppu.main_screen&16)) return packet;
    constexpr unsigned sizes[6][2]{{8,16},{8,32},{8,64},{16,32},{16,64},{32,64}};
    const auto selection=std::min(unsigned(ppu.object_select>>5),5U);
    for(unsigned object=128;object-->0;) {
        const auto low=object*4;const auto attr=ppu.oam[low+3];
        if(!(ppu.oam[low]|ppu.oam[low+1]|ppu.oam[low+2]|attr)) continue;
        if(ppu.oam[low]==248 && ppu.oam[low+1]==248) continue;
        if(priority && ((attr>>4)&3)!=*priority) continue;
        const auto high=ppu.oam[512+object/4]>>((object%4)*2);
        int x=ppu.oam[low]+((high&1)<<8);if(x>=256) x-=512;
        const unsigned glyph=ppu.oam[low+2]&0x7f;
        const bool boss_label=ppu.oam[low+1]<32 && glyph>=0x71 && glyph<=0x74;
        if(boss_label && meters) {
            if(!meters->enabled || !meters->boss_max_health) continue;
            const unsigned maximum=meters->boss_max_health;
            const unsigned span=(maximum&0x80)?maximum>>1:maximum;
            x=256-18-int(span+4)-33+int(glyph-0x71)*8;
        }
        const int size=int(sizes[selection][(high>>1)&1]);
        const int left=std::max(x,0),right=std::min(x+size,256);
        if(right<=left) continue;
        for(int wrap=0;wrap<2;++wrap) {
            const int y=int(ppu.oam[low+1])+(boss_label?1:0)-wrap*256;
            const int top=std::max(y,0),bottom=std::min(y+size,224);
            if(bottom<=top) continue;
            const int corners[4][2]{{left,top},{right,top},{right,bottom},{left,bottom}};
            for(unsigned corner:{0U,1U,2U,0U,2U,3U}) {
                SceneVertex v{};v.position[0]=float(corners[corner][0]);v.position[1]=float(corners[corner][1]);
                v.uv[0]=float(corners[corner][0]-x);v.uv[1]=float(corners[corner][1]-y);
                v.texture[1]=ppu.object_select|((uint32_t(ppu.oam[low+2])|((attr&1U)<<8))<<8);
                v.texture[2]=attr|(uint32_t(size)<<8);v.texture[3]=16|(srgb?2:0);
                packet.geometry.vertices.push_back(v);
            }
        }
    }
    if(packet.geometry.vertices.empty()) return packet;
    auto& data=packet.geometry.texels;data.resize(272+16384);data[13]=15-brightness;
    const auto palette=render::decode_bgr555_palette(ppu.cgram);
    for(unsigned i=0;i<256;++i) data[16+i]=palette[i].r|(uint32_t(palette[i].g)<<8)|(uint32_t(palette[i].b)<<16)|0xff000000U;
    pack_vram(ppu.vram,std::span<uint32_t,16384>(data.data()+272,16384));
    return packet;
}
}
