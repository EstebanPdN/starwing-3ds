#include "starfox/assets/rom.hpp"
#include "starfox/audio/spc700_audio.hpp"
#include "starfox/simulation/game_simulation.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>

namespace {
std::atomic<std::size_t> allocations{};
bool count_allocations{};
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}
template<class T> void write(std::ofstream& stream, std::span<const T> values) {
    stream.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size_bytes()));
}
}

void* operator new(std::size_t bytes) {
    if (count_allocations) ++allocations;
    if (auto* pointer = std::malloc(bytes == 0 ? 1 : bytes)) return pointer;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }

// Build once with the pre-change SPC source and -DSTARFOX_AUDIO_BASELINE,
// then with the candidate source. The complete binary transcripts must match.
// This host correctness test measures allocations, not 3DS frame time.
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        const auto rom = starfox::assets::RomImage::load(argv[1]);
        const auto symbols = starfox::assets::SymbolMap::load(argv[2]);
        auto game = std::make_unique<starfox::simulation::GameSimulation>(
            rom, symbols, "LEVEL1_1", std::span<const std::uint8_t>{}, true);
        const auto boot = game->map().take_apu_port_writes();
        starfox::audio::Spc700Audio audio;
        (void)audio.prime_upload_sequence(boot);
        check(audio.driver_loaded(), "Fixture did not load the SPC driver");
        std::ofstream transcript{argv[3], std::ios::binary};
        std::vector<std::int16_t> output;
        std::uint32_t rng = 0x6842de71U;
        unsigned audible_ticks{};
        for (unsigned tick = 0; tick < 512; ++tick) {
            rng = rng * 1664525U + 1013904223U;
            std::array<starfox::simulation::ApuPortWrite, 4> writes{{
                {1, static_cast<std::uint8_t>(rng), 0},
                {2, static_cast<std::uint8_t>(rng >> 8U), 512},
                {3, static_cast<std::uint8_t>(tick % 11U == 0 ? 0x35 : 0), 4096},
                {0, static_cast<std::uint8_t>(tick < 256 ? 0x01 : 0x08), 8192},
            }};
            if (tick % 64U == 32) writes[2].value = 0x01;
            if (tick % 64U == 48) writes[2].value = 0x02;
            if (tick == 400) writes[3].value = 0x05;
            allocations = 0;
            count_allocations = tick > 0;
#ifdef STARFOX_AUDIO_BASELINE
            output = audio.render_logic_tick(writes);
#else
            audio.render_logic_tick(writes, output);
#endif
            count_allocations = false;
            check(output.size() == 3200, "Wrong packet size");
            for (const auto sample : output) {
                if (sample != 0) { ++audible_ticks; break; }
            }
#ifndef STARFOX_AUDIO_BASELINE
            check(allocations == 0, "SPC packet allocated after warmup");
#endif
            write(transcript, std::span<const std::int16_t>{output});
            write(transcript, audio.last_music_samples());
            write(transcript, audio.last_effect_samples());
            const auto ports = audio.output_ports();
            write(transcript, std::span<const std::uint8_t>{ports});
            const auto saved = audio.save_state();
            write(transcript, std::span<const std::uint8_t>{saved});
            if (tick == 128 || tick == 384) audio.load_state(saved);
        }
        check(transcript.good(), "Transcript write failed");
        check(audible_ticks > 10, "Fixture did not exercise audible synthesis");
        std::cout << "512 ticks: PCM, both stems, ports and complete SPC states recorded; "
            << "steady allocations/tick=" << allocations << "; audible ticks="
            << audible_ticks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
