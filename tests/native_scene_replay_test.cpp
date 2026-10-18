#include <3ds.h>
#include "frame_3ds.hpp"
#include "diagnostic_sha256.hpp"
#include "starfox/render/palette.hpp"
#include <fstream>
#include "starfox/audio/spc700_audio.hpp"
#include "starfox/input/buttons.hpp"
#include <array>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
using namespace starfox;
namespace {
template<class T> void hash(platform_3ds::DiagnosticSha256& sum,std::span<const T> values) {
    sum.update({reinterpret_cast<const std::uint8_t*>(values.data()),values.size_bytes()});
}
unsigned current_frame{};
void same(const render::Framebuffer& expected,const render::Framebuffer& actual,bool layered) {
    if(expected.pixels()!=actual.pixels()
        || (layered && !std::equal(expected.write_coverage().begin(),expected.write_coverage().end(),
            actual.write_coverage().begin(),actual.write_coverage().end())))
        throw std::runtime_error("Reference fallback/stereo frame differs at frame "+std::to_string(current_frame)
            +" pixels_equal="+std::to_string(expected.pixels()==actual.pixels())
            +" expected_coverage="+std::to_string(expected.write_coverage().size())
            +" actual_coverage="+std::to_string(actual.write_coverage().size()));
}
}
int main(int argc,char** argv) try {
    if(argc!=6&&argc!=7)return 2;
    const auto rom=assets::RomImage::load(argv[1]);const auto symbols=assets::SymbolMap::load(argv[2]);
    const auto count=std::stoul(argv[4]);const bool split=std::string(argv[5])=="new";
    auto game=std::make_unique<simulation::GameSimulation>(rom,symbols,argv[3],std::span<const std::uint8_t>{},true);
    game->set_god_mode(true);
    audio::Spc700Audio sound(split?audio::Spc700Audio::Routing::split_stems:audio::Spc700Audio::Routing::cartridge);
    (void)sound.prime_upload_sequence(game->map().take_apu_port_writes());
    for(unsigned i=0;i<30;++i)(void)sound.render_logic_tick({});
    game->synchronize_apu_output_ports(sound.output_ports());
    auto reference=std::make_unique<platform_3ds::Frame3ds>(rom,symbols);
    auto candidate=std::make_unique<platform_3ds::Frame3ds>(rom,symbols);
    reference->set_top_hud_visible(false);candidate->set_top_hud_visible(false);candidate->set_native_engine(true);
    platform_3ds::DiagnosticSha256 audio_hash;
    std::array<bool,16> modes{};std::array<bool,32> flows{};
    bool exported=false;
    std::uint64_t native{},fallback{},stereo{},rectangles{},source_commands{},sampled{},reference_ns{},candidate_ns{};
    for(unsigned n=0;n<count;++n) {
        current_frame=n;
        const auto held=static_cast<input::ButtonMask>(input::b|(n%160<80?input::left:input::right));
        const auto tick=game->tick({held,0,0});
        const auto pcm=sound.render_logic_tick(tick.audio_port_writes);
        hash(audio_hash,std::span<const std::int16_t>{pcm});
        const auto ports=sound.output_ports();hash(audio_hash,std::span<const std::uint8_t>{ports});
        if(n%31==0){const auto state=sound.save_state();hash(audio_hash,std::span<const std::uint8_t>{state});}
        game->synchronize_apu_output_ports(ports);
        reference->capture_after_tick(*game);candidate->capture_after_tick(*game);
        const auto mode=game->map().ppu_state().background_mode;
        if(mode<modes.size())modes[mode]=true;
        const auto flow=unsigned(game->flow_state());if(flow<flows.size())flows[flow]=true;
        const unsigned strength=split && n%240>=180?8U:0U;
        reference->set_stereo_strength(strength);candidate->set_stereo_strength(strength);
        const double alpha=std::array{0.0,0.5,1.0}[n%3];
        auto begin=svcGetSystemTick();const auto& expected=reference->draw(*game,true,alpha);reference_ns+=svcGetSystemTick()-begin;
        begin=svcGetSystemTick();const auto& actual=candidate->draw(*game,true,alpha);candidate_ns+=svcGetSystemTick()-begin;
        if(const auto* scene=candidate->native_scene()) {
            std::vector<std::uint8_t> world(400*240);
            for(const auto& r:scene->rectangles())for(int y=r.top;y<r.bottom;++y)for(int x=r.left;x<r.right;++x)
                world[y*400+x]=(((x/r.scale)^(y/r.scale))&1)?r.odd:r.even;
            for(std::size_t i=0;i<world.size();++i) {
                const auto pixel=actual.write_coverage()[i]?actual.pixels()[i]:world[i];
                const bool coverage=actual.write_coverage()[i]||world[i]!=0;
                if(pixel!=expected.pixels()[i] || coverage!=bool(expected.write_coverage()[i]))
                    throw std::runtime_error("Native pixel/coverage differs at frame "+std::to_string(n)+" pixel "+std::to_string(i));
            }
            if(argc==7 && !exported && n>=count/2) {
                std::ofstream file(argv[6],std::ios::binary);
                const std::uint32_t size=scene->rectangles().size();
                file.write(reinterpret_cast<const char*>(&size),4);
                file.write(reinterpret_cast<const char*>(scene->rectangles().data()),size*sizeof(platform_3ds::native_engine::Rectangle));
                const auto palette=render::decode_bgr555_palette(game->map().ppu_state().cgram);
                file.write(reinterpret_cast<const char*>(palette.data()),sizeof(palette));
                file.write(reinterpret_cast<const char*>(world.data()),world.size());
                if(!file)throw std::runtime_error("GPU fixture write failed");
                exported=true;
            }
            ++native;rectangles+=scene->rectangles().size();source_commands+=scene->stats().source_commands;sampled+=scene->stats().sampled_pixels;
        } else {++fallback;same(expected,actual,mode==2U && (game->flow_state()==simulation::GameFlowState::gameplay || game->flow_state()==simulation::GameFlowState::training));}
        if(reference->stereo_active()) {++stereo;same(reference->right_frame(),candidate->right_frame(),true);}
    }
    std::ostringstream digest;for(auto x:audio_hash.finish())digest<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(x);
    std::cout<<"{\"stage\":\""<<argv[3]<<"\",\"model\":\""<<argv[5]<<"\",\"frames\":"<<count
        <<",\"native_frames\":"<<native<<",\"fallback_frames\":"<<fallback<<",\"stereo_frames\":"<<stereo
        <<",\"rectangles\":"<<rectangles<<",\"source_commands\":"<<source_commands<<",\"sampled_pixels\":"<<sampled
        <<",\"reference_ms\":"<<reference_ns/1e6<<",\"candidate_ms\":"<<candidate_ns/1e6
        <<",\"native_audio_blocks\":"<<sound.native_blocks_executed()<<",\"audio_state_sha256\":\""<<digest.str()<<"\",\"modes\":[";
    bool comma=false;for(unsigned i=0;i<modes.size();++i)if(modes[i]){if(comma)std::cout<<',';std::cout<<i;comma=true;}
    std::cout<<"],\"flows\":[";comma=false;for(unsigned i=0;i<flows.size();++i)if(flows[i]){if(comma)std::cout<<',';std::cout<<i;comma=true;}
    std::cout<<"],\"pixel_differences\":0,\"coverage_differences\":0,\"physical_hardware\":false}\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
