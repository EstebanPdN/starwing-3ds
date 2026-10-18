#include "options_menu_3ds.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace starfox;
using namespace starfox::platform_3ds;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
void image(const char* file,const std::vector<std::uint32_t>& pixels) {
    std::ofstream f(file,std::ios::binary);f<<"P6\n320 240\n255\n";
    for(auto c:pixels) {char p[]{char(c>>24),char(c>>16),char(c>>8)};f.write(p,3);}
}
int main(int argc,char** argv) {
    if(argc!=4) return 2;
    try {
        const auto rom=assets::RomImage::load(argv[1]);
        const auto symbols=assets::SymbolMap::load(argv[2]);
        std::filesystem::create_directories(argv[3]);std::filesystem::current_path(argv[3]);
        std::filesystem::create_directories("sdmc:/3ds/Starwing");
        std::filesystem::remove("sdmc:/3ds/Starwing/options-old3ds.cfg");
        {std::ofstream f("sdmc:/3ds/Starwing/options.cfg");f<<"volume=70\noriginal_speed=0\ninterpolation=1\n";}
        OptionsMenu3ds old(rom,symbols,false,"",hardware_profile(false));
        require(old.original_speed()&&!old.interpolate()&&old.volume()==70,"Old profile defaults/fallback wrong");
        (void)old.touch(160,95,true);(void)old.touch(160,85,true);
        require(old.page()==MenuPage::gameplay,"Gameplay navigation failed");
        old.draw_options();image("gameplay.ppm",old.bottom_pixels());
        (void)old.touch(160,180,true);require(old.page()==MenuPage::timing,"Timing entry failed");
        (void)old.touch(200,134,true);require(old.interpolate()&&old.original_speed(),"Original+interpolation unavailable");
        old.draw_options();image("timing-original-on.ppm",old.bottom_pixels());
        (void)old.touch(200,80,true);require(!old.original_speed(),"20 Hz touch failed");
        (void)old.navigate(1);(void)old.navigate(-1);(void)old.activate();require(old.original_speed(),"D-pad speed selection failed");
        (void)old.navigate(1);(void)old.activate();require(!old.interpolate(),"D-pad interpolation selection failed");
        require(old.fps_limit()==0U,"FPS limit must default off");
        (void)old.touch(160,188,true);require(old.fps_limit()==30U,"30 FPS touch failed");
        old.draw_options();image("timing-30.ppm",old.bottom_pixels());
        (void)old.touch(240,188,true);require(old.fps_limit()==60U,"60 FPS touch failed");
        old.draw_options();image("timing-60.ppm",old.bottom_pixels());
        (void)old.activate();require(old.fps_limit()==0U,"FPS cycle failed");
        (void)old.activate();require(old.fps_limit()==30U,"FPS cycle to 30 failed");
        old.draw_options();image("timing-original-off.ppm",old.bottom_pixels());
        (void)old.back();require(old.page()==MenuPage::gameplay,"Timing back lost parent");
        OptionsMenu3ds resumed(rom,symbols,false,"",hardware_profile(false));
        require(resumed.original_speed()&&!resumed.interpolate()&&resumed.fps_limit()==30U,"Old preferences did not persist");
        OptionsMenu3ds newer(rom,symbols,false,"",hardware_profile(true));
        require(!newer.original_speed()&&newer.interpolate()&&newer.fps_limit()==0U,"Old settings changed New profile");
        require(hardware_profile(false).default_fps_limit==0U&&hardware_profile(true).default_fps_limit==0U,"Profile targets wrong");
        std::cout<<"PASS: model defaults, old-file migration, original+interpolation, touch/D-pad, persistence and profile isolation\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
