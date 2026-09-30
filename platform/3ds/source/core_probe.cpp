#include "core_probe.h"
#include "asset_stage.hpp"

#include "starfox/assets/rom.hpp"
#include "starfox/simulation/game_simulation.hpp"

#include <3ds.h>

#include <cstdio>
#include <exception>
#include <utility>

extern "C" int starwing_core_probe(const char *bundle_path,
    char *message, size_t capacity) {
    try {
        auto payload = starwing_load_runtime_payload(bundle_path);
        const starfox::assets::RomImage rom{std::move(payload.original_rom)};
        const auto symbols = starfox::assets::SymbolMap::parse(
            payload.original_symbols);
        const auto begin = osGetTime();
        starfox::simulation::GameSimulation game{
            rom, symbols, "LEVEL1_1", {}, true};
        for (unsigned tick = 0; tick < 20; ++tick) {
            for (unsigned video = 0; video < 3; ++video)
                game.present_frame();
            static_cast<void>(game.tick({}));
        }
        const auto elapsed = osGetTime() - begin;
        std::snprintf(message, capacity,
            "ARM32 core: 20 ticks OK\n%lu ms init + simulation\n"
            "%lu active objects\nRender/audio still pending",
            static_cast<unsigned long>(elapsed),
            static_cast<unsigned long>(game.objects().active_count()));
        return 1;
    } catch (const std::exception &error) {
        std::snprintf(message, capacity, "ARM32 core failed:\n%s", error.what());
        return 0;
    }
}
