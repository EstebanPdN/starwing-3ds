#include "options_menu_3ds.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace starfox;
using namespace starfox::platform_3ds;
void require(bool ok,const char* m){if(!ok)throw std::runtime_error(m);}
void image(const std::string& name,const std::vector<std::uint32_t>& pixels,int w){
    std::ofstream f(name+".ppm",std::ios::binary);f<<"P6\n"<<w<<" 240\n255\n";
    for(auto c:pixels){char p[]{char(c>>24),char(c>>16),char(c>>8)};f.write(p,3);}
}
int main(int argc,char** argv){
    if(argc!=4&&argc!=5)return 2;
    try{
        const auto rom=assets::RomImage::load(argv[1]);const auto symbols=assets::SymbolMap::load(argv[2]);
        const auto retail=argc==5?std::filesystem::absolute(argv[4]):std::filesystem::path{};
        std::filesystem::create_directories(argv[3]);std::filesystem::current_path(argv[3]);
        std::filesystem::create_directories("sdmc:/3ds/Starwing");
        if(!retail.empty())for(int n=0;n<6;++n)std::filesystem::copy_file(retail,
            "sdmc:/3ds/Starwing/STAR FOX TEST "+std::to_string(n)+".sfc",std::filesystem::copy_options::overwrite_existing);
        OptionsMenu3ds m(rom,symbols,false,"",hardware_profile(false));
        auto snap=[&](const char* n){m.draw_options();image(std::string(n)+"-top",m.top_pixels(),400);image(std::string(n)+"-bottom",m.bottom_pixels(),320);};
        auto root=[&]{m.set_update_status({});m.close();(void)m.touch(160,95,true);require(m.page()==MenuPage::root,"Root entry failed");};
        m.draw_title_bottom(0);const auto title_on=m.bottom_pixels();
        m.draw_title_bottom(499);require(m.bottom_pixels()==title_on,"Title changed within the same blink phase");
        m.draw_title_bottom(500);const auto title_off=m.bottom_pixels();
        require(title_off!=title_on,"Title prompt did not blink");
        m.draw_title_bottom(999);require(m.bottom_pixels()==title_off,"Title off phase was not static");
        m.draw_title_bottom(1000);require(m.bottom_pixels()==title_on,"Title blink period changed");
        root();snap("root");(void)m.touch(160,50,true);require(m.page()==MenuPage::display,"Display entry failed");snap("display");
        (void)m.touch(100,90,true);require(!m.wide(),"Aspect touch missed");(void)m.touch(200,165,true);require(!m.top_hud(),"HUD touch missed");
        root();(void)m.touch(160,85,true);snap("gameplay");
        (void)m.touch(200,81,true);require(m.volume()>65&&m.volume()<80,"Volume touch missed moved slider");
        (void)m.touch(200,146,true);require(m.auto_save(),"Auto-save touch missed");
        (void)m.touch(160,180,true);require(m.page()==MenuPage::timing,"Timing touch missed");
        (void)m.touch(100,90,true);(void)m.touch(200,165,true);require(m.original_speed()&&m.interpolate(),"Original with interpolation unavailable");snap("timing");
        root();(void)m.touch(160,115,true);require(m.page()==MenuPage::developer,"Developer entry failed");snap("developer");
        (void)m.touch(160,110,true);require(m.show_fps(),"FPS touch missed");(void)m.touch(160,165,true);require(m.overlay_enabled(),"Overlay touch missed");
        root();(void)m.touch(160,178,true);require(m.page()==MenuPage::rom_mode,"Version entry failed");snap("version");
        (void)m.touch(160,115,true);require(m.confirming(),"EX selection missed");snap("confirm-version");m.cancel_confirmation();
        (void)m.touch(160,174,true);require(m.page()==MenuPage::rom_selection,"ROM picker entry failed");snap("rom-picker");
        if(!retail.empty()){
            (void)m.touch(255,215,true);require(m.page()==MenuPage::rom_selection,"Next exited picker");snap("rom-picker-page2");
            (void)m.touch(60,215,true);require(m.page()==MenuPage::rom_selection,"Previous exited picker");
        }
        m.close();(void)m.touch(160,140,true);require(m.confirming(),"Main-menu confirmation entry failed");m.draw_entry();image("confirm-main-bottom",m.bottom_pixels(),320);m.cancel_confirmation();
        root();require(m.touch(160,145,true)==MenuAction::check_update,"Update entry failed");
        std::string notes="## Changelog\n";for(int i=0;i<24;++i)notes+="- Corrección de audio, interfaz y velocidad original.\n";m.set_update_notes(notes);
        UpdateStatus status;std::strcpy(status.version,"v1.1-E2");
        struct Case{const char* name;UpdateState state;const char* message;};
        const Case cases[]={{"checking",UpdateState::checking,"CHECKING FOR UPDATES"},{"empty",UpdateState::empty,"NO EXPERIMENTAL RELEASE"},{"current",UpdateState::current,"YOU ARE UP TO DATE"},{"available",UpdateState::available,"UPDATE AVAILABLE"},{"downloading",UpdateState::downloading,"DOWNLOADING"},{"installing",UpdateState::installing,"INSTALLING - DO NOT POWER OFF"},{"done",UpdateState::done,"UPDATED - REOPEN STARWING 3DS"},{"error",UpdateState::error,"HTTP 403: CHECK CONNECTION / TRY AGAIN"}};
        for(const auto& c:cases){status.state=c.state;status.percent=57;std::strcpy(status.message,c.message);m.set_update_status(status);snap(c.name);}
        status.state=UpdateState::available;status.experimental=true;std::strcpy(status.message,"UPDATE AVAILABLE");m.set_update_status(status);snap("experimental");
        (void)m.touch(255,215,true);require(m.page()==MenuPage::update,"Next exited updater");snap("notes-next");
        (void)m.touch(60,215,true);require(m.page()==MenuPage::update,"Previous exited updater");
        (void)m.touch(160,176,true);require(m.confirming(),"Install touch missed moved row");snap("confirm-update");
        require(m.activate_confirmation()==MenuAction::none,"Install default was not No");
        status.state=UpdateState::installing;m.set_update_status(status);require(m.touch(160,215,true)==MenuAction::none,"Install safety bypassed");
        status.state=UpdateState::downloading;m.set_update_status(status);require(m.touch(160,215,true)==MenuAction::cancel_update,"Cancel touch missed");
        std::cout<<"PASS: all Options pages, moved touch targets, ROM and changelog paging, eight updater states and confirmation/cancel safety\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
