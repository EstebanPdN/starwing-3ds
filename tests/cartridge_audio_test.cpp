#include "starfox/assets/rom.hpp"
#include "starfox/audio/spc700_audio.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "starfox/input/buttons.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace starfox;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main(int argc,char** argv) {
    if(argc!=4) return 2;
    try {
        const auto rom=assets::RomImage::load(argv[1]);
        const auto symbols=assets::SymbolMap::load(argv[2]);
        auto game=std::make_unique<simulation::GameSimulation>(rom,symbols,"LEVEL1_1",std::span<const std::uint8_t>{},true);
#ifdef STARFOX_AUDIO_REFERENCE
        audio::Spc700Audio audio;
#else
        audio::Spc700Audio audio{audio::Spc700Audio::Routing::cartridge};
#endif
        const auto boot=game->map().take_apu_port_writes();
        require(audio.prime_upload_sequence(boot)>0,"Boot upload missing");
        require(audio.driver_loaded(),"Cartridge driver not loaded");
        for(unsigned i=0;i<30;++i) (void)audio.render_logic_tick({});
        game->synchronize_apu_output_ports(audio.output_ports());
        for(unsigned i=0;i<2;++i) {
            const auto tick=game->tick({});
            (void)audio.render_logic_tick(tick.audio_port_writes);
            game->synchronize_apu_output_ports(audio.output_ports());
        }
        require(game->map().apu_upload_generation()>=2U,"Stage bank not exercised");
        std::ofstream f(argv[3],std::ios::binary);
        std::vector<std::int16_t> pcm;
        unsigned audible{};
        for(unsigned tick=0;tick<512;++tick) {
            std::array<simulation::ApuPortWrite,4> writes{{
                {1,static_cast<std::uint8_t>(tick),0},
                {2,static_cast<std::uint8_t>(tick>>1),512},
                {3,static_cast<std::uint8_t>(tick%19==0?0x35:0),4096},
                {0,static_cast<std::uint8_t>(tick<256?1:8),8192}}};
            if(tick%64==32) writes[2].value=1;
            if(tick%64==48) writes[2].value=2;
            audio.render_logic_tick(writes,pcm);
#ifdef STARFOX_AUDIO_REFERENCE
            const auto samples=audio.last_music_samples();
#else
            const std::span<const std::int16_t> samples{pcm};
            require(audio.last_effect_samples().empty(),"Single profile synthesized a second stem");
#endif
            require(samples.size()==3200,"Packet length changed");
            audible+=std::any_of(samples.begin(),samples.end(),[](auto x){return x!=0;});
            f.write(reinterpret_cast<const char*>(samples.data()),samples.size_bytes());
            const auto ports=audio.output_ports();
            f.write(reinterpret_cast<const char*>(ports.data()),ports.size());
            if(tick==128||tick==384) {
                const auto saved=audio.save_state();
                audio::Spc700Audio restored;
                restored.load_state(saved);
                require(restored.save_state()==saved,"Restore changed SPC state");
                const auto expected=audio.render_logic_tick({});
                const auto actual=restored.render_logic_tick({});
                require(expected==actual&&audio.output_ports()==restored.output_ports(),"Restore continuation changed audio");
                audio.load_state(saved);
                auto broken=saved;broken.back()^=1;
                bool rejected=false;
                try {audio.load_state(broken);} catch(const std::exception&){rejected=true;}
                require(rejected&&audio.save_state()==saved,"Invalid state mutated SPC");
            }
        }
        game=std::make_unique<simulation::GameSimulation>(rom,symbols,"LEVEL1_1",std::span<const std::uint8_t>{},true);
#ifdef STARFOX_AUDIO_REFERENCE
        audio=audio::Spc700Audio{};
#else
        audio=audio::Spc700Audio{audio::Spc700Audio::Routing::cartridge};
#endif
        (void)audio.prime_upload_sequence(game->map().take_apu_port_writes());
        for(unsigned i=0;i<30;++i) (void)audio.render_logic_tick({});
        game->synchronize_apu_output_ports(audio.output_ports());
        unsigned commands{},gameplay{};
        for(unsigned tick=0;tick<256;++tick) {
            const auto held=static_cast<input::ButtonMask>(input::b|(tick%64<32?input::left:input::right));
            const auto press=static_cast<input::ButtonMask>((tick==96||tick==128)?input::start:0);
            const auto result=game->tick({static_cast<input::ButtonMask>(held|press),press,0});
            commands+=!result.audio_port_writes.empty();
            gameplay+=game->flow_state()==simulation::GameFlowState::gameplay;
            audio.render_logic_tick(result.audio_port_writes,pcm);
            game->synchronize_apu_output_ports(audio.output_ports());
#ifdef STARFOX_AUDIO_REFERENCE
            const auto samples=audio.last_music_samples();
#else
            const std::span<const std::int16_t> samples{pcm};
#endif
            f.write(reinterpret_cast<const char*>(samples.data()),samples.size_bytes());
            const auto ports=audio.output_ports();f.write(reinterpret_cast<const char*>(ports.data()),ports.size());
        }
        require(gameplay>50&&commands>10,"Real gameplay/audio path was not exercised");
        require(audible>10&&f.good(),"Fixture did not produce audible synthesis");
        std::cout<<"512 controlled + 256 gameplay packets; audible="<<audible<<"; port timing and restore exercised\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
