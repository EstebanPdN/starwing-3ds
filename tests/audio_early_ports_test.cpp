#include "starfox/assets/rom.hpp"
#include "starfox/audio/spc700_audio.hpp"
#include "starfox/input/buttons.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace starfox;
using Clock = std::chrono::steady_clock;
struct Published {
    std::array<std::uint8_t,4> ports{};
    Clock::time_point at{};
    unsigned calls{};
    static void ready(void* context, const std::array<std::uint8_t,4>& value) noexcept {
        auto& self = *static_cast<Published*>(context);
        self.ports=value; self.at=Clock::now(); ++self.calls;
    }
};
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) try {
    if (argc!=3) return 2;
    const auto rom=assets::RomImage::load(argv[1]);
    const auto symbols=assets::SymbolMap::load(argv[2]);
    unsigned packets{}, audible{};
    double cpu_us{}, total_us{};
    for (const auto routing:{audio::Spc700Audio::Routing::cartridge,
                             audio::Spc700Audio::Routing::split_stems}) {
        for (const char* level:{"LEVEL1_1","LEVEL1_2","LEVEL1_3","LEVEL1_4","LEVEL1_5"}) {
            auto game=std::make_unique<simulation::GameSimulation>(
                rom,symbols,level,std::span<const std::uint8_t>{},true);
            audio::Spc700Audio reference(routing),candidate(routing);
            const auto boot=game->map().take_apu_port_writes();
            require(reference.prime_upload_sequence(boot)==candidate.prime_upload_sequence(boot),"Upload cadence differs");
            require(reference.driver_loaded() && candidate.driver_loaded(),"Boot did not load a driver");
            std::vector<std::int16_t> expected,actual;
            for (unsigned warmup=0;warmup<30;++warmup) {
                reference.render_logic_tick({},expected);
                candidate.render_logic_tick({},actual);
            }
            for (unsigned tick=0;tick<256;++tick) {
                game->synchronize_apu_output_ports(reference.output_ports());
                const auto result=game->tick({static_cast<std::uint16_t>(input::b
                    | (tick%64<32?input::left:input::right)),0,0});
                const auto& writes=result.audio_port_writes;
                reference.render_logic_tick(writes,expected);
                Published published;
                const auto begin=Clock::now();
                candidate.render_logic_tick_with_ports(writes,actual,&Published::ready,&published);
                const auto end=Clock::now();
                require(published.calls==1,"Ports must publish exactly once");
                require(published.ports==reference.output_ports()
                    && published.ports==candidate.output_ports(),"Early ports differ from completed ports");
                require(expected==actual,"Early publication changed PCM");
                if (tick%17==0 || tick==255)
                    require(reference.save_state()==candidate.save_state(),"Early publication changed full SPC state");
                for (auto sample:actual) if(sample) {++audible;break;}
                if (routing==audio::Spc700Audio::Routing::cartridge) {
                    cpu_us+=std::chrono::duration<double,std::micro>(published.at-begin).count();
                    total_us+=std::chrono::duration<double,std::micro>(end-begin).count();
                }
                ++packets;
            }
        }
    }
    require(audible>128,"Fixtures did not exercise audible synthesis");
    std::printf("PASS packets=%u audible=%u cartridge_ports_us=%.3f cartridge_total_us=%.3f trailing_dsp_filter_percent=%.2f\n",
        packets,audible,cpu_us/1280,total_us/1280,100*(total_us-cpu_us)/total_us);
} catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}
