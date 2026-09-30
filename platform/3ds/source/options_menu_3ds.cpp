#include "options_menu_3ds.hpp"
#include "bottom_hud_art_data.hpp"
#include "entry_panel_art_data.hpp"
#include "fps_panel_art_data.hpp"
#include "rom_panel_art_data.hpp"
#include "rom_probe.h"
#include "starfield_3ds.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>

namespace starfox::platform_3ds {
namespace {
static_assert([] {
    std::size_t pixels=0U;
    for(const auto run:entry_panel_runs) pixels+=run&0x0fffU;
    return pixels==320U*240U;
}());
constexpr std::uint32_t rgb(unsigned r,unsigned g,unsigned b) {
    return (r<<24U)|(g<<16U)|(b<<8U)|255U;
}
constexpr auto bg=rgb(41,56,74), black=rgb(0,0,0), navy=rgb(18,30,46);
constexpr auto white=rgb(255,255,255), ice=rgb(156,255,255);
constexpr auto steel=rgb(164,182,197), blue=rgb(106,149,156);
constexpr auto grey=rgb(123,121,131), dark=rgb(90,89,98);
constexpr auto muted=rgb(98,133,131);
constexpr const char* config_path="sdmc:/3ds/Starwing/options.cfg";
std::uint32_t symbol(const assets::SymbolMap& map,const char* name) {
    const auto& found=map.find(name);
    return found.empty()?0U:found.front();
}
}

OptionsMenu3ds::OptionsMenu3ds(const assets::RomImage& rom,
    const assets::SymbolMap& symbols,bool current_ex,
    const std::string& current_rom_path):rom_(rom),game_text_(rom,symbols),
    bomb_count_address_(symbol(symbols,"SPECWEPCNTONE")?
        symbol(symbols,"SPECWEPCNTONE"):symbol(symbols,"SPECWEPCNT")),
    font_glyphs_(symbol(symbols,"FONT0FON")),
    font_widths_(symbol(symbols,"FONT0WID")),
    font_translation_(symbol(symbols,"FONT0TRN")),
    current_ex_(current_ex),selected_rom_path_(current_rom_path),
    current_rom_path_(current_rom_path),top_(400U*240U),bottom_(320U*240U) { load(); }

void OptionsMenu3ds::load() {
    std::FILE* file=std::fopen(config_path,"rb");
    if (!file) return;
    char line[80]{};
    while (std::fgets(line,sizeof(line),file)) {
        unsigned n{};
        if (std::sscanf(line,"wide=%u",&n)==1) wide_=n!=0;
        else if (std::sscanf(line,"top_hud=%u",&n)==1) top_hud_=n!=0;
        else if (std::sscanf(line,"volume=%u",&n)==1) volume_=std::min(n,100U);
        else if (std::sscanf(line,"auto_save=%u",&n)==1) auto_save_=n!=0;
        else if (std::sscanf(line,"show_fps=%u",&n)==1) show_fps_=n!=0;
        else if (std::sscanf(line,"overlay=%u",&n)==1) overlay_enabled_=n!=0;
    }
    std::fclose(file);
}
void OptionsMenu3ds::save() const {
    std::FILE* file=std::fopen(config_path,"wb");
    if (!file) return;
    std::fprintf(file,"wide=%u\ntop_hud=%u\nvolume=%u\nauto_save=%u\nshow_fps=%u\noverlay=%u\n",
        unsigned(wide_),unsigned(top_hud_),volume_,unsigned(auto_save_),
        unsigned(show_fps_),unsigned(overlay_enabled_));
    std::fclose(file);
}
void OptionsMenu3ds::rect(std::vector<std::uint32_t>& target,int width,
    int x,int y,int w,int h,std::uint32_t color) {
    if(w<=0||h<=0) return;
    const int x0=std::clamp(x,0,width),x1=std::clamp(x+w,0,width);
    const int y0=std::clamp(y,0,240),y1=std::clamp(y+h,0,240);
    for(int yy=y0;yy<y1;++yy)
        std::fill(target.begin()+yy*width+x0,target.begin()+yy*width+x1,color);
}
void OptionsMenu3ds::glyph(std::vector<std::uint32_t>& target,int screen_width,
    char character,int x,int y,int scale,std::uint32_t color) {
    if(character<'!'||character>'Z'||!font_glyphs_||!font_widths_||!font_translation_) return;
    const auto translated=rom_.read8(font_translation_+unsigned(character-' '));
    const auto width=std::min<unsigned>(rom_.read8(font_widths_+translated),16U);
    const auto base=font_glyphs_+unsigned(translated)*24U;
    for(int row=0;row<12;++row) {
        const auto bits=rom_.read16(base+unsigned(row)*2U);
        for(unsigned col=0;col<width;++col)
            if(bits&(0x8000U>>col))
                rect(target,screen_width,x+int(col)*scale,y+row*scale,
                    scale,scale,color);
    }
}
int OptionsMenu3ds::text_width(const char* value,int scale) const {
    int width=0;
    for(const char* p=value;*p;++p) {
        if(*p==' ') width+=5*scale;
        else if(font_widths_&&font_translation_&&*p>='!'&&*p<='Z') {
            const auto index=rom_.read8(font_translation_+unsigned(*p-' '));
            width+=(std::min<unsigned>(rom_.read8(font_widths_+index),16U)+1U)*scale;
        }
    }
    return width?width-scale:0;
}
void OptionsMenu3ds::text(std::vector<std::uint32_t>& target,int screen_width,
    const char* value,int x,int y,int scale,std::uint32_t color) {
    for(const char* p=value;*p;++p) {
        if(*p==' ') {x+=5*scale;continue;}
        glyph(target,screen_width,*p,x,y,scale,color);
        if(font_widths_&&font_translation_&&*p>='!'&&*p<='Z') {
            const auto index=rom_.read8(font_translation_+unsigned(*p-' '));
            x+=int(std::min<unsigned>(rom_.read8(font_widths_+index),16U)+1U)*scale;
        }
    }
}
void OptionsMenu3ds::centered(std::vector<std::uint32_t>& target,int screen_width,
    const char* value,int center_x,int y,int scale,std::uint32_t color) {
    text(target,screen_width,value,center_x-text_width(value,scale)/2,y,scale,color);
}
int OptionsMenu3ds::small_text_width(const char* value) const {
    int result=0;
    for(const char* p=value;*p;++p) {
        if(*p==' ') result+=4;
        else if(font_widths_&&font_translation_&&*p>='!'&&*p<='Z') {
            const auto index=rom_.read8(font_translation_+unsigned(*p-' '));
            const auto width=std::min<unsigned>(rom_.read8(font_widths_+index),16U);
            result+=int((width*2U+1U)/3U)+1;
        }
    }
    return result?result-1:0;
}
void OptionsMenu3ds::small_text(std::vector<std::uint32_t>& target,
    int screen_width,const char* value,int center_x,int y,std::uint32_t color) {
    int x=center_x-small_text_width(value)/2;
    for(const char* p=value;*p;++p) {
        if(*p==' ') {x+=4;continue;}
        if(!font_glyphs_||!font_widths_||!font_translation_
            ||*p<'!'||*p>'Z') continue;
        const auto index=rom_.read8(font_translation_+unsigned(*p-' '));
        const auto width=std::min<unsigned>(rom_.read8(font_widths_+index),16U);
        if(width==0U) continue;
        const auto small_width=(width*2U+1U)/3U;
        const auto base=font_glyphs_+unsigned(index)*24U;
        for(unsigned row=0;row<8U;++row) {
            const auto source_row=(row*12U+4U)/8U;
            const auto bits=rom_.read16(base+source_row*2U);
            for(unsigned col=0;col<small_width;++col) {
                const auto source_col=std::min(width-1U,(col*3U+1U)/2U);
                if(bits&(0x8000U>>source_col))
                    rect(target,screen_width,x+int(col),y+int(row),1,1,color);
            }
        }
        x+=int(small_width)+1;
    }
}
void OptionsMenu3ds::button(int x,int y,int w,int h,const char* label,
    bool selected,bool enabled) {
    const auto face=selected?blue:grey;
    rect(bottom_,320,x,y,w,h,enabled?steel:dark);
    rect(bottom_,320,x+1,y,w-2,2,enabled?white:steel);
    rect(bottom_,320,x+1,y+2,2,h-4,enabled?white:steel);
    rect(bottom_,320,x+3,y+3,w-6,h-6,face);
    rect(bottom_,320,x+2,y+h-3,w-4,2,dark);
    centered(bottom_,320,label,x+w/2,y+(h-12)/2,1,enabled?ice:steel);
}
void OptionsMenu3ds::plate() {
    std::fill(bottom_.begin(),bottom_.end(),bg);
    rect(bottom_,320,14,23,292,193,white);
    rect(bottom_,320,16,26,288,187,steel);
    rect(bottom_,320,18,29,284,179,blue);
    rect(bottom_,320,21,33,278,169,navy);
    rect(bottom_,320,23,36,274,162,black);
    for(int x:{32,40,48,272,280,288})
        rect(bottom_,320,x,31,3,3,x==32||x==288?muted:ice);
    for(int x:{11,302}) {
        rect(bottom_,320,x,91,8,30,dark);
        rect(bottom_,320,x+2,95,4,8,ice);
        rect(bottom_,320,x+2,108,4,8,ice);
    }
    rect(bottom_,320,105,202,110,12,white);
    rect(bottom_,320,109,205,102,7,navy);
}
int OptionsMenu3ds::row_count() const {
    switch(page_) {
    case MenuPage::root:return 6;
    case MenuPage::display:return 3;
    case MenuPage::gameplay:return 3;
    case MenuPage::developer:return 4;
    case MenuPage::update:return 2;
    case MenuPage::rom_selection:return static_cast<int>(roms_.size())+1;
    case MenuPage::rom_mode:return 4;
    default:return 0;
    }
}
void OptionsMenu3ds::scan_roms() {
    roms_.clear();
    if(auto* directory=opendir("sdmc:/3ds/Starwing")) {
        while(auto* entry=readdir(directory)) {
            if(!starwing_has_rom_extension(entry->d_name)) continue;
            const std::string path=std::string("sdmc:/3ds/Starwing/")
                +entry->d_name;
            StarwingRomInfo info{};
            if(starwing_probe_rom(path.c_str(),&info)) {
                std::string label=entry->d_name;
                std::transform(label.begin(),label.end(),label.begin(),
                    [](unsigned char c) {return static_cast<char>(std::toupper(c));});
                roms_.push_back({std::move(label),path});
            }
        }
        closedir(directory);
    }
    std::sort(roms_.begin(),roms_.end(),[](const auto& a,const auto& b) {
        return a.label<b.label;
    });
    selection_=0;
    rom_scroll_=0;
}
MenuAction OptionsMenu3ds::hit_row(int row) {
    if(page_==MenuPage::root) {
        if(row==5) {back();return MenuAction::none;}
        page_=row==4?MenuPage::rom_mode
            :static_cast<MenuPage>(int(MenuPage::display)+row);
        selection_=0;
        return MenuAction::none;
    }
    if(row==row_count()-1) {back();return MenuAction::none;}
    switch(page_) {
    case MenuPage::display:
        if(row==0) wide_=!wide_;
        if(row==1) top_hud_=!top_hud_;
        save();break;
    case MenuPage::gameplay:
        if(row==0) volume_=(volume_+10U)%110U;
        if(row==1) auto_save_=!auto_save_;
        save();break;
    case MenuPage::developer:
        if(row==0) return MenuAction::memory_dump;
        if(row==1) show_fps_=!show_fps_;
        if(row==2) overlay_enabled_=!overlay_enabled_;
        save();break;
    case MenuPage::rom_selection:
        if(row>=0&&row<static_cast<int>(roms_.size())) {
            selected_rom_path_=roms_[static_cast<std::size_t>(row)].path;
            page_=MenuPage::rom_mode;
            selection_=0;
        }
        break;
    case MenuPage::rom_mode:
        if(row==2) {page_=MenuPage::rom_selection;scan_roms();break;}
        selected_rom_ex_=row==1;
        if(startup_selection_) {
            startup_selection_=false;
            return MenuAction::launch_rom;
        }
        if(selected_rom_path_==current_rom_path_&&selected_rom_ex_==current_ex_)
            break;
        confirming_version_=true;
        confirmation_yes_=false;
        break;
    default:break; // Update remains a placeholder until a hosted manifest exists.
    }
    return MenuAction::none;
}
MenuAction OptionsMenu3ds::navigate(int delta) {
    if(!open()) return MenuAction::none;
    selection_=(selection_+delta+row_count())%row_count();
    if(page_==MenuPage::rom_selection
        &&selection_<static_cast<int>(roms_.size())) {
        if(selection_<rom_scroll_) rom_scroll_=selection_;
        if(selection_>=rom_scroll_+4) rom_scroll_=selection_-3;
    }
    return MenuAction::none;
}
MenuAction OptionsMenu3ds::activate() {
    return open()?hit_row(selection_):MenuAction::none;
}
MenuAction OptionsMenu3ds::activate_confirmation() noexcept {
    if(!confirming()) return MenuAction::none;
    const auto action=confirming_main_menu_?MenuAction::main_menu
        :MenuAction::launch_rom;
    cancel_confirmation();
    return confirmation_yes_?action:MenuAction::none;
}
void OptionsMenu3ds::back() {
    if(confirming()) {cancel_confirmation();return;}
    if(page_==MenuPage::root) page_=MenuPage::closed;
    else if(page_==MenuPage::rom_mode && startup_selection_) {
        page_=MenuPage::rom_selection;
    }
    else if(page_==MenuPage::rom_selection) {
        page_=startup_selection_?MenuPage::closed:MenuPage::rom_mode;
        startup_selection_=false;
    }
    else if(open()) page_=MenuPage::root;
    selection_=0;
}
void OptionsMenu3ds::open_rom_picker() {
    startup_selection_=true;
    page_=MenuPage::rom_selection;
    selection_=0;
    rom_scroll_=0;
    scan_roms();
}
MenuAction OptionsMenu3ds::touch(int x,int y,bool controls_screen) {
    if(confirming()) {
        if(y>=125&&y<159&&x>=57&&x<149) {
            confirmation_yes_=true;
            return activate_confirmation();
        }
        if(y>=125&&y<159&&x>=171&&x<263) {
            cancel_confirmation();
        }
        return MenuAction::none;
    }
    if(!open()) {
        if(!controls_screen||x<70||x>250) return MenuAction::none;
        if(y>=85&&y<112) {page_=MenuPage::root;selection_=0;return MenuAction::none;}
        if(y>=129&&y<156) {
            confirming_main_menu_=true;
            confirmation_yes_=false;
            return MenuAction::none;
        }
        return MenuAction::none;
    }
    if(x<27||x>293) return MenuAction::none;
    if(y>=203&&y<229) {back();return MenuAction::none;}
    if(page_==MenuPage::root) {
        for(int row=0;row<5;++row)
            if(y>=41+row*31&&y<67+row*31) {selection_=row;return hit_row(row);}
    } else if(page_==MenuPage::display) {
        if(y>=79&&y<105) {
            if(x>=64&&x<156) wide_=false;
            else if(x>=164&&x<256) wide_=true;
            save();selection_=0;return MenuAction::none;
        }
        if(y>=153&&y<179) {
            if(x>=64&&x<156) top_hud_=true;
            else if(x>=164&&x<256) top_hud_=false;
            save();selection_=1;return MenuAction::none;
        }
        if(y>=48&&y<75) {selection_=0;return hit_row(0);}
        if(y>=122&&y<149) {selection_=1;return hit_row(1);}
    } else if(page_==MenuPage::gameplay) {
        if(y>=85&&y<105&&x>=64&&x<256) {
            volume_=static_cast<unsigned>(std::clamp((x-67)*100/186,0,100));
            save();selection_=0;return MenuAction::none;
        }
        if(y>=48&&y<75) {selection_=0;return hit_row(0);}
        if(y>=122&&y<149) {selection_=1;return hit_row(1);}
        if(y>=153&&y<179) {
            auto_save_=x>=164&&x<256;
            save();selection_=1;return MenuAction::none;
        }
    } else if(page_==MenuPage::developer) {
        for(int row=0;row<3;++row)
            if(y>=48+row*43&&y<79+row*43) {selection_=row;return hit_row(row);}
    } else if(page_==MenuPage::rom_selection) {
        if(y>=187&&y<203) {
            if(x>=37&&x<129) rom_scroll_=std::max(0,rom_scroll_-4);
            if(x>=191&&x<283) rom_scroll_=std::min(
                std::max(0,static_cast<int>(roms_.size())-4),rom_scroll_+4);
            return MenuAction::none;
        }
        for(int visible=0;visible<4;++visible) {
            const int row=rom_scroll_+visible;
            if(row<static_cast<int>(roms_.size())
                &&y>=43+visible*37&&y<73+visible*37) {
                selection_=row;return hit_row(row);
            }
        }
    } else if(page_==MenuPage::rom_mode) {
        if(y>=65&&y<99) {selection_=0;return hit_row(0);}
        if(y>=108&&y<142) {selection_=1;return hit_row(1);}
        if(y>=151&&y<185) {selection_=2;return hit_row(2);}
    }
    return MenuAction::none;
}
void OptionsMenu3ds::draw_entry(bool map_stars,std::uint32_t sky) {
    // Match the actual upper-screen sky for this menu. The old entry path
    // painted black, while the unrelated title starfield also painted black.
    // Both controls and planet selection use this same cartridge sky style.
    (void)map_stars;
    if(sky != 0U) entry_sky_=sky;
    std::fill(bottom_.begin(),bottom_.end(),entry_sky_);
    constexpr std::array<std::array<int,3>,29> stars{{
        {17,19,0},{49,30,1},{83,13,0},{112,45,0},{157,23,1},
        {207,31,0},{264,17,0},{300,39,1},{24,65,1},{74,80,0},
        {132,68,0},{180,91,1},{235,77,0},{286,99,0},{41,116,0},
        {102,126,1},{157,111,0},{221,129,0},{279,143,1},
        {13,159,0},{64,175,1},{124,162,0},{192,183,0},
        {255,169,1},{305,191,0},{38,214,0},{91,226,1},
        {207,217,0},{279,228,1}}};
    for(const auto& star:stars) {
        const int x=star[0],y=star[1];
        const auto arm=rgb(131,149,131);
        if(star[2]) {
            rect(bottom_,320,x-1,y,3,1,arm);
            rect(bottom_,320,x,y-1,1,3,arm);
            rect(bottom_,320,x,y,1,1,rgb(189,206,189));
        } else rect(bottom_,320,x,y,1,1,arm);
    }
    // Python-drawn pixel art: the same flat rectangular menu frame and
    // palette as the existing Options root, around the two entry buttons.
    std::size_t pixel=0U;
    for(const auto run:entry_panel_runs) {
        const auto colour=entry_panel_colors[run>>12U];
        const auto length=static_cast<std::size_t>(run&0x0fffU);
        const auto alpha=colour&255U;
        if(alpha==255U) {
            std::fill_n(bottom_.begin()+pixel,length,colour);
        } else if(alpha!=0U) {
            for(std::size_t i=0;i<length;++i) {
                const auto under=bottom_[pixel+i];
                const auto blend=[&](unsigned shift) {
                    const auto upper=(colour>>shift)&255U;
                    const auto lower=(under>>shift)&255U;
                    return (upper*alpha+lower*(255U-alpha)+127U)/255U;
                };
                bottom_[pixel+i]=(blend(24U)<<24U)|(blend(16U)<<16U)
                    |(blend(8U)<<8U)|255U;
            }
        }
        pixel+=length;
    }
    if(confirming_main_menu_) {
        rect(bottom_,320,35,64,250,117,steel);
        rect(bottom_,320,38,67,244,111,navy);
        centered(bottom_,320,"ARE YOU SURE?",160,83,1,white);
        centered(bottom_,320,"PROGRESS MAY BE LOST",160,105,1,steel);
        button(57,125,92,34,"YES",confirmation_yes_);
        button(171,125,92,34,"NO",!confirmation_yes_);
        return;
    }
    button(70,85,180,27,"OPTIONS");
    button(70,129,180,27,"MAIN MENU");
}

void OptionsMenu3ds::draw_title_top(const render::Framebuffer& frame,
    const render::Palette256& palette) {
    if(frame.width()!=400U||frame.height()!=240U) return;
    for(std::size_t i=0;i<top_.size();++i) {
        const auto& c=palette[frame.pixels()[i]];
        top_[i]=rgb(c.r,c.g,c.b);
    }
    // Copyright's original raster is 7 rows with two-pixel stems. The N,
    // i, n, t, e, d and o below follow its actual pixel rows at y=208..214;
    // the other letters are drawn on that same grid and stroke weight.
    struct TitleGlyph {
        int width;
        std::array<std::uint8_t,8> rows;
    };
    const auto small_glyph=[](char c)->TitleGlyph {
        switch(c) {
        case 'P':return {7,{0,0b1111110,0b1100011,0b1111110,
                            0b1100000,0b1100000,0b1100000,0}};
        case 'D':return {7,{0,0b1111100,0b1100110,0b1100011,
                            0b1100011,0b1100110,0b1111100,0}};
        case 'N':return {7,{0,0b1100011,0b1110011,0b1111011,
                            0b1101111,0b1100111,0b1100011,0}};
        case 'E':return {6,{0,0b111111,0b110000,0b111110,
                            0b110000,0b110000,0b111111,0}};
        case 'o':return {6,{0,0,0b011110,0b110011,0b110011,0b110011,
                            0b011110,0}};
        case 'r':return {5,{0,0,0b11011,0b11100,0b11000,0b11000,
                            0b11000,0}};
        case 't':return {4,{0,0b0110,0b1111,0b0110,0b0110,0b0110,
                            0b0110,0}};
        case 'e':return {6,{0,0,0b011110,0b110011,0b111111,0b110000,
                            0b011110,0}};
        case 'd':return {6,{0,0b000011,0b011111,0b110011,0b110011,
                            0b110011,0b011111,0}};
        case 'b':return {6,{0,0b110000,0b111110,0b110011,
                            0b110011,0b110011,0b111110,0}};
        case 'y':return {6,{0,0,0b110011,0b110011,0b110011,0b011111,
                            0b000011,0b011110}};
        case 's':return {6,{0,0,0b011111,0b110000,0b011110,0b000011,
                            0b111110,0}};
        case 'a':return {6,{0,0,0b011110,0b000011,0b011111,0b110011,
                            0b011111,0}};
        case 'n':return {6,{0,0,0b111110,0b110011,0b110011,0b110011,
                            0b110011,0}};
        case ' ':return {4,{}};
        default:return {4,{}};
        }
    };
    constexpr char credit[]="Ported by Esteban PDN";
    int width=0;
    for(const char* letter=credit;*letter;++letter)
        width+=small_glyph(*letter).width+1;
    int x=(400-width+1)/2;
    for(const char* letter=credit;*letter;++letter) {
        const auto glyph=small_glyph(*letter);
        for(int row=0;row<8;++row) for(int col=0;col<glyph.width;++col)
            if(glyph.rows[static_cast<std::size_t>(row)]
                &(1U<<(glyph.width-1-col)))
                rect(top_,400,x+col,219+row,1,1,white);
        x+=glyph.width+1;
    }
}

void OptionsMenu3ds::draw_space_bottom() {
    std::fill(bottom_.begin(),bottom_.end(),black);
    // Isolated star coordinates and five-pixel crosses from the actual
    // upper title sky in dump 000, mapped to the lower screen width.
    constexpr std::array<std::array<int,4>,31> stars{{
        {66,11,0,1},{168,11,0,1},{8,12,0,0},{276,22,0,0},
        {91,33,1,1},{245,35,0,1},{42,37,0,0},{302,48,0,0},
        {232,51,0,1},{72,57,1,1},{21,67,1,0},{117,75,0,1},
        {281,82,1,0},{74,91,0,1},{232,97,1,1},{142,99,0,1},
        {49,101,0,0},{117,107,0,1},{200,107,0,1},{312,122,0,0},
        {91,129,1,1},{245,137,1,1},{11,151,0,0},{72,153,1,1},
        {271,159,0,0},{232,163,0,1},{245,179,0,1},{37,186,0,0},
        {299,194,0,0},{18,224,0,0},{316,225,0,0}}};
    for(const auto& star:stars) {
        const auto [x,y,cross,warm]=star;
        const auto color=warm?rgb(255,149,98):rgb(139,141,139);
        if(cross) for(const auto& delta:
            std::array<std::array<int,2>,4>{{{0,-1},{0,1},{-1,0},{1,0}}})
            rect(bottom_,320,x+delta[0],y+delta[1],1,1,color);
        rect(bottom_,320,x,y,1,1,color);
    }
}

void OptionsMenu3ds::draw_game_over_bottom() {
    static const auto stars=[] {
        std::vector<std::uint32_t> result(320U*240U,black);
        for(unsigned y=0;y<240U;++y) for(unsigned x=0;x<320U;++x) {
            if(!game_over_star(x,y,0x1b873593U)) continue;
            const auto shade=game_over_star_hash(x,y,0x1b873593U)>>12U;
            const auto color=shade%53U==0U?rgb(255,222,65)
                :shade%47U==0U?rgb(213,121,82)
                :shade%41U==0U?rgb(82,165,255)
                :shade%2U==0U?rgb(246,255,222):rgb(164,174,148);
            result[std::size_t(y)*320U+x]=color;
        }
        return result;
    }();
    bottom_=stars;
}

void OptionsMenu3ds::draw_black() {
    std::fill(top_.begin(),top_.end(),black);
    std::fill(bottom_.begin(),bottom_.end(),black);
}

void OptionsMenu3ds::draw_title_bottom(std::uint64_t time_ms) {
    draw_space_bottom();
    if((time_ms/500U)%2U==0U)
        centered(bottom_,320,"PUSH START",160,103,1,ice);
    centered(bottom_,320,"HOME MENU",160,215,1,steel);
}

void OptionsMenu3ds::draw_level_hud(
    const simulation::GameSimulation& game,
    const render::Palette256& palette,float fps) {
    static const auto background=[] {
        std::vector<std::uint32_t> decoded(320U*240U);
        std::size_t cursor=0;
        for(const auto run:bottom_hud_runs) {
            const auto color=bottom_hud_colors[run>>12U];
            const auto count=std::size_t(run&0x0fffU);
            std::fill_n(decoded.begin()+cursor,count,color);
            cursor+=count;
        }
        // Prepare these interiors once, not on every presented frame.
        for(auto& pixel:decoded)
            if(pixel==rgb(12,22,36)) pixel=black;
        return decoded;
    }();
    bottom_=background;
    // The source HUD art has two very dark blue empty screens. Keep every
    // outline pixel, but make the portrait and message interiors truly black.
    const auto meter=game.peek_meter_state();
    hud_status_frame_.clear(0U);
    hud_sprites_.draw_objects(game.map().ppu_state(),hud_status_frame_,
        std::nullopt,0,true,false,nullptr,false,&meter,true,true);
    if(meter.enabled) hud_sprites_.draw_meters(meter,hud_status_frame_);
    const auto copy_cartridge_status=[&](int source_x,int source_y,
        int width,int height,int destination_x,int destination_y,int scale=1) {
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            const auto index=hud_status_frame_.get(source_x+x,source_y+y);
            if(index==0U) continue;
            const auto& c=palette[index];
            const int dx=destination_x+x*scale,dy=destination_y+y*scale;
            if(dx>=0&&dx<320&&dy>=0&&dy<240)
                rect(bottom_,320,dx,dy,scale,scale,rgb(c.r,c.g,c.b));
        }
    };
    // Pixel-sampled from the cartridge FONT0, with equal 8-pixel height and
    // centered in the separate 45/40-pixel label sockets.
    // Solid sockets and a fixed 5x7 bitmap keep these short captions crisp;
    // shrinking FONT0 by point sampling loses the B and S strokes.
    rect(bottom_,320,34,191,47,11,black);
    rect(bottom_,320,210,191,44,11,black);
    const auto fixed_label=[&](const char* value,int center_x) {
        const int length=int(std::strlen(value));
        int x=center_x-(length*6-1)/2;
        for(const char* p=value;*p;++p,x+=6) {
            std::array<unsigned char,7> rows{};
            switch(*p) {
            case 'S': rows={15,16,16,14,1,1,30};break;
            case 'H': rows={17,17,17,31,17,17,17};break;
            case 'I': rows={31,4,4,4,4,4,31};break;
            case 'E': rows={31,16,16,30,16,16,31};break;
            case 'L': rows={16,16,16,16,16,16,31};break;
            case 'D': rows={30,17,17,17,17,17,30};break;
            case 'B': rows={30,17,17,30,17,17,30};break;
            case 'O': rows={14,17,17,17,17,17,14};break;
            case 'M': rows={17,27,21,21,17,17,17};break;
            default:break;
            }
            for(int row=0;row<7;++row)
                for(int col=0;col<5;++col)
                    if(rows[unsigned(row)]&(1U<<(4-col)))
                        rect(bottom_,320,x+col,193+row,1,1,white);
        }
    };
    fixed_label("SHIELD",57);
    fixed_label("BOMBS",231);
    rect(bottom_,320,251,192,1,9,steel);
    rect(bottom_,320,252,193,1,7,muted);
    const auto& oam=game.map().ppu_state().oam;
    unsigned bomb_slot=0;
    for(unsigned object=0;object<128;++object) {
        const auto base=object*4U;
        const auto tile=unsigned(oam[base+2U])|((oam[base+3U]&1U)<<8U);
        if(tile==188U && bomb_slot<5U) {
            // Isolate each tile; sampling the composite status framebuffer
            // can pick up a neighbouring glyph on the third bomb.
            bomb_icon_frame_.clear(0U);
            hud_sprites_.draw_oam_icon(game.map().ppu_state(),object,
                bomb_icon_frame_);
            for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
                const auto index=bomb_icon_frame_.get(x,y);
                if(index==0U) continue;
                const auto& c=palette[index];
                rect(bottom_,320,256+int(bomb_slot)*8+x,193+y,1,1,
                    rgb(c.r,c.g,c.b));
            }
            ++bomb_slot;
        }
        if(tile==189U && object+2U<128U) {
            // A one-pixel increase across the 24-pixel life readout is the
            // smallest legible step at native bottom-screen resolution.
            for(int dx=0;dx<25;++dx) {
                const auto part=unsigned(dx*24/25)/8U;
                const auto sx=unsigned(dx*24/25)%8U;
                const auto record=(object+part)*4U;
                copy_cartridge_status(oam[record]+int(sx),oam[record+1U],
                    1,8,19+dx,14);
            }
        }
    }
    // The original meter state and palette drive the two authored sockets.
    const auto health_max=meter.extended?std::max(1,int(meter.player_health_max)):36;
    const auto& red=palette[meter.shield_up?119U:114U];
    const auto& blue_meter=palette[118U];
    rect(bottom_,320,27,210,59*std::clamp(int(meter.damage),0,health_max)/health_max,
        7,rgb(red.r,red.g,red.b));
    rect(bottom_,320,219,210,59*std::clamp(int(meter.boost),0,36)/36,
        7,rgb(blue_meter.r,blue_meter.g,blue_meter.b));
    const auto dialogue=game.dialogue_state();
    if(dialogue.active) {
        portrait_frame_.clear(0U);
        game_text_.draw_face(dialogue.portrait_frame,0,0,portrait_frame_,
            7U*16U,dialogue.alternate_portraits);
        for(int sy=0;sy<40;++sy) for(int sx=0;sx<32;++sx) {
            const auto index=portrait_frame_.get(sx,sy);
            if(index==0U) continue;
            const auto& c=palette[index];
            rect(bottom_,320,128+sx*2,127+sy*2,2,2,rgb(c.r,c.g,c.b));
        }
        if(dialogue.meter_visible) {
            rect(bottom_,320,134,113,52,8,steel);
            rect(bottom_,320,136,115,48,4,dark);
            rect(bottom_,320,136,115,
                std::min<int>(48,int(dialogue.meter_health)*48/40),4,
                rgb(205,40,98));
        }
        if(dialogue.text_visible&&dialogue.text_address) {
            hud_text_frame_.clear(0U);
            // Draw original ROM glyphs at source resolution, then point
            // sample them at 5:4. This remains real pixel art and allows
            // two or three lines inside the shorter message frame.
            game_text_.draw_game_text(dialogue.text_address,0,0,
                hud_text_frame_,7U*16U,std::nullopt,208,72U);
            for(int dy=0;dy<45;++dy) for(int dx=0;dx<260;++dx) {
                const auto index=hud_text_frame_.get(dx*4/5,dy*4/5);
                if(index==0U) continue;
                const auto& c=palette[index];
                bottom_[std::size_t(48+dy)*320U+unsigned(29+dx)]=rgb(c.r,c.g,c.b);
            }
        }
    }
    if(show_fps_) {
        char label[24]{};
        std::snprintf(label,sizeof(label),"%.1f FPS",fps);
        std::size_t cursor=0;
        for(const auto run:fps_panel_runs) {
            const auto color=fps_panel_colors[run>>12U];
            for(unsigned n=0;n<(run&0x0fffU);++n,++cursor)
                if((color&255U)!=0U)
                    bottom_[(8U+cursor/77U)*320U+227U+cursor%77U]=color;
        }
        centered(bottom_,320,label,265,13,1,ice);
    }
}
void OptionsMenu3ds::draw_options() {
    std::fill(top_.begin(),top_.end(),bg);
    if(page_==MenuPage::rom_selection||page_==MenuPage::rom_mode) {
        std::fill(top_.begin(),top_.end(),black);
        constexpr std::array<std::array<int,2>,19> stars{{
            {8,12},{45,18},{81,8},{124,32},{183,15},{238,22},{294,11},
            {349,27},{388,13},{5,81},{394,91},{10,143},{391,163},
            {4,218},{57,232},{137,226},{247,231},{338,223},{395,228}}};
        for(const auto& star:stars)
            rect(top_,400,star[0],star[1],1,1,steel);
        std::size_t cursor=0;
        for(const auto run:rom_panel_runs) {
            const auto color=rom_panel_colors[run>>12U];
            const auto length=std::size_t(run&0x0fffU);
            if((color&255U)!=0U)
                std::fill_n(top_.begin()+cursor,length,color);
            cursor+=length;
        }
        centered(top_,400,page_==MenuPage::rom_mode?"VERSION":"ROMS",
            89,36,1,ice);
        if(page_==MenuPage::rom_selection) {
            if(roms_.empty()) centered(top_,400,"NO SUPPORTED ROMS",200,128,1,steel);
            for(int visible=0;visible<4;++visible) {
                const int row=rom_scroll_+visible;
                if(row>=static_cast<int>(roms_.size())) break;
                auto label=roms_[static_cast<std::size_t>(row)].label;
                while(label.size()>1U&&text_width(label.c_str())>298)
                    label.pop_back();
                text(top_,400,label.c_str(),47,82+visible*28,1,
                    selection_==row?ice:white);
            }
        } else {
            centered(top_,400,current_ex_?"PLAYING STAR FOX EX":"PLAYING ORIGINAL",
                200,91,1,white);
            centered(top_,400,"ORIGINAL",200,131,1,ice);
            centered(top_,400,"STAR FOX EX",200,166,1,ice);
        }
    } else {
        centered(top_,400,"OPTIONS",200,105,2,white);
    }
    plate();
    if(confirming_version_) {
        rect(bottom_,320,35,64,250,117,steel);
        rect(bottom_,320,38,67,244,111,navy);
        centered(bottom_,320,"SWITCH GAME VERSION?",160,83,1,white);
        centered(bottom_,320,"PROGRESS MAY BE LOST",160,105,1,steel);
        button(57,125,92,34,"YES",confirmation_yes_);
        button(171,125,92,34,"NO",!confirmation_yes_);
        return;
    }
    if(page_==MenuPage::root) {
        const char* labels[]{"DISPLAY","GAMEPLAY","DEVELOPER","UPDATE","GAME VERSION"};
        for(int row=0;row<5;++row)
            button(30,41+row*31,260,26,labels[row],selection_==row);
    } else if(page_==MenuPage::display) {
        button(37,48,246,26,"ASPECT RATIO",selection_==0);
        button(64,79,92,26,"ORIGINAL",false,!wide_);
        button(164,79,92,26,"WIDE",false,wide_);
        button(37,122,246,26,"TOP HUD",selection_==1);
        button(64,153,92,26,"SHOW",false,top_hud_);
        button(164,153,92,26,"HIDE",false,!top_hud_);
    } else if(page_==MenuPage::gameplay) {
        button(37,48,246,26,"MASTER VOLUME",selection_==0);
        rect(bottom_,320,64,88,192,12,steel);
        rect(bottom_,320,67,91,186,6,dark);
        rect(bottom_,320,67,91,186*int(volume_)/100,6,ice);
        button(37,122,246,26,"AUTO SAVE",selection_==1);
        button(64,153,92,26,"OFF",false,!auto_save_);
        button(164,153,92,26,"ON",false,auto_save_);
    } else if(page_==MenuPage::developer) {
        button(37,48,246,27,"MEMORY DUMP",selection_==0);
        button(37,91,246,27,show_fps_?"SHOW FPS ON":"SHOW FPS OFF",selection_==1);
        button(37,134,246,27,overlay_enabled_?"OVERLAY ON":"OVERLAY OFF",selection_==2);
    } else if(page_==MenuPage::update) {
        button(37,72,246,27,"CHECK UPDATE",false,false);
        centered(bottom_,320,"COMING SOON",160,118,1,steel);
    } else if(page_==MenuPage::rom_selection) {
        if(roms_.empty()) centered(bottom_,320,"NO SUPPORTED ROMS",160,95,1,steel);
        for(int visible=0;visible<4;++visible) {
            const int row=rom_scroll_+visible;
            if(row>=static_cast<int>(roms_.size())) break;
            auto label=roms_[static_cast<std::size_t>(row)].label;
            while(label.size()>1U&&text_width(label.c_str())>222)
                label.pop_back();
            button(37,43+visible*37,246,30,label.c_str(),selection_==row);
        }
        if(roms_.size()>4U) {
            button(37,187,92,16,"PREV");
            button(191,187,92,16,"NEXT");
        }
    } else if(page_==MenuPage::rom_mode) {
        button(53,65,214,34,"ORIGINAL",selection_==0);
        button(53,108,214,34,"STAR FOX EX",selection_==1);
        button(53,151,214,34,"ROM FILES",selection_==2);
    }
    button(110,204,100,23,"BACK",selection_==row_count()-1);
}

void OptionsMenu3ds::draw_overlay(const OverlayMetrics& metrics) {
    std::fill(bottom_.begin(),bottom_.end(),bg);
    auto frame=[&](int x,int y,int w,int h) {
        rect(bottom_,320,x,y,w,h,muted);
        rect(bottom_,320,x+1,y,w-2,2,white);
        rect(bottom_,320,x,y+2,2,h-4,steel);
        rect(bottom_,320,x+w-2,y+2,2,h-4,steel);
        rect(bottom_,320,x+2,y+h-2,w-4,2,blue);
        rect(bottom_,320,x+3,y+3,w-6,h-6,navy);
        rect(bottom_,320,x+3,y+3,w-6,1,blue);
    };
    frame(8,5,304,25);
    frame(8,38,104,89);
    frame(119,38,193,89);
    frame(8,138,304,34);
    for(int x:{8,112,216}) frame(x,181,96,50);
    for(int y:{80,94}) {
        rect(bottom_,320,6,y,4,7,muted);
        rect(bottom_,320,7,y+1,2,5,ice);
        rect(bottom_,320,310,y,4,7,muted);
        rect(bottom_,320,311,y+1,2,5,ice);
    }
    text(bottom_,320,"STARWING 3DS",15,11,1,white);
    text(bottom_,320,"ESTEBAN PDN",150,11,1,steel);
    char version[16]{};
    std::snprintf(version,sizeof(version),"V%s",
        metrics.build_version?metrics.build_version:"?");
    text(bottom_,320,version,270,11,1,white);
    text(bottom_,320,"PERFORMANCE",15,44,1,white);
    char value[80]{};
    std::snprintf(value,sizeof(value),"%.1f",metrics.fps);
    text(bottom_,320,value,24,61,2,white);
    text(bottom_,320,"PRESENT FPS",17,90,1,steel);
    std::snprintf(value,sizeof(value),"LOGIC %.1fHZ",metrics.logic_hz);
    text(bottom_,320,value,15,108,1,white);
    text(bottom_,320,"MEMORY",126,44,1,white);
    std::snprintf(value,sizeof(value),"HEAP FREE %uKB",metrics.heap_free_kib);
    text(bottom_,320,value,126,60,1,steel);
    rect(bottom_,320,126,76,177,5,muted);
    rect(bottom_,320,127,77,metrics.heap_total_kib==0?0:
        std::min(175U,175U*metrics.heap_free_kib/metrics.heap_total_kib),3,ice);
    std::snprintf(value,sizeof(value),"LINEAR FREE %uKB",metrics.linear_free_kib);
    text(bottom_,320,value,126,87,1,steel);
    rect(bottom_,320,126,103,177,5,muted);
    rect(bottom_,320,127,104,metrics.linear_total_kib==0?0:
        std::min(175U,175U*metrics.linear_free_kib/metrics.linear_total_kib),3,ice);
    text(bottom_,320,metrics.new_3ds?"NEW 3DS 400X240 PICA":"OLD 3DS 400X240 PICA",
        126,112,1,white);
    std::snprintf(value,sizeof(value),"CSND %s CORE %d BG2 %s %s",
        metrics.audio_ready?"ON":"OFF",metrics.audio_core,
        metrics.gpu_bg2?"PICA":"CPU",metrics.flow?metrics.flow:"BOOT");
    text(bottom_,320,value,15,143,1,ice);
    text(bottom_,320,"L+R+SELECT TO CLOSE",15,157,1,white);
    const char* labels[]{"QUICK DUMP","FULL MEMORY","CLEAR DUMPS"};
    const char* combos[]{"L+R+A","L+R+B","L+R+X"};
    for(int n=0;n<3;++n) {
        const int x=8+n*104;
        centered(bottom_,320,labels[n],x+48,190,1,white);
        centered(bottom_,320,combos[n],x+48,212,1,ice);
    }
}
} // namespace starfox::platform_3ds
