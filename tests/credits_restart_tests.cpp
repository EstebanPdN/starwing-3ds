#include "starfox/assets/rom.hpp"
#include "starfox/input/buttons.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc,char** argv) try {
    if(argc!=3) {std::cerr<<"Usage: credits_restart_tests ROM SYMBOLS\n";return 2;}
    const auto rom=starfox::assets::RomImage::load(argv[1]);
    const auto symbols=starfox::assets::SymbolMap::load(argv[2]);
    auto game=std::make_unique<starfox::simulation::GameSimulation>(rom,symbols,"CREDITSMAP");
    game->set_timing_mode(starfox::simulation::TimingMode::unlocked_20_fps);
    using Flow=starfox::simulation::GameFlowState;
    const bool extended=!symbols.find("SPECWEPCNTONE").empty();
    for(unsigned tick=0;tick<20000;++tick) {
        // Repeated/held Start covers the exact transfer in which THE END
        // finishes its wait, not only a host flag set manually in a fixture.
        for(unsigned phase=0;phase<3;++phase) game->present_frame();
        (void)game->tick({starfox::input::start,starfox::input::start,0});
        const auto flow=game->flow_state();
        if(flow==Flow::credits || flow==Flow::finished) continue;
        if(flow!=(extended?Flow::ex_pregame_menu:Flow::intro))
            throw std::runtime_error("Credits Start entered a corrupt/wrong frontend");
        if(!extended && game->map().read_native_byte(symbols.find("BG2VOFSOVERRIDE").front())!=0)
            throw std::runtime_error("Credits restart retained its background override");
        // Exercise the returned frontend too, including another source tick.
        (void)game->tick({});
        if(!extended) {
            bool playable=false;
            for(unsigned step=0;step<5000 && !playable;++step) {
                starfox::input::ButtonMask held=starfox::input::start;
                const auto next=game->flow_state();
                if(next==Flow::controls_choice) held=starfox::input::down|starfox::input::a;
                else if(next==Flow::planet_select || next==Flow::planet_travel) held=starfox::input::a;
                for(unsigned phase=0;phase<3;++phase) game->present_frame();
                (void)game->tick({held,held,0});
                playable=game->flow_state()==Flow::gameplay && game->objects().is_active(game->player());
            }
            if(!playable) throw std::runtime_error("Post-credits frontend could not launch a new playable game");
        }
        std::cout<<"Credits Start returned safely after "<<tick+1<<" source ticks\n";return 0;
    }
    throw std::runtime_error("Credits restart did not finish in bounded source time");
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
