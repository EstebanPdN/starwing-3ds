#include "../platform/3ds/source/frame_3ds.hpp"
#include "starfox/assets/rom.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include "starfox/render/palette.hpp"
#include "starfox/input/buttons.hpp"
#include <memory>
#include <stdexcept>

int main(int argc, char** argv) try {
    if (argc != 3 && argc != 4) throw std::runtime_error("expected patched ROM and symbols");
    const auto rom = starfox::assets::RomImage::load(argv[1]);
    const auto symbols = starfox::assets::SymbolMap::load(argv[2]);
    unsigned mismatches = 0;
    for (const char* scene : {"INTROMAP", "TITLEMAP", "CONTMAP", "GAMEOVER", "MAP1_1A"}) {
        auto game = std::make_unique<starfox::simulation::GameSimulation>(rom, symbols, scene);
        // Exercise both initial objects and later source animation/objects.
        auto persistent = std::make_unique<starfox::platform_3ds::Frame3ds>(rom, symbols);
        const auto catalog = persistent->preload_catalog(12U * 1024U * 1024U);
        std::cout << scene << " catalog_decoded=" << catalog.decoded
            << " catalog_bytes=" << catalog.bytes << " limited=" << catalog.limited
            << " rejected=" << catalog.rejected << '\n';
        if (!catalog.decoded || catalog.bytes > 12U * 1024U * 1024U)
            throw std::runtime_error("catalog preload did not honor its storage budget");
        for (unsigned tick = 0; tick < 180; ++tick) {
            starfox::input::TickInput input{};
            if (std::string_view(scene) == "MAP1_1A" && tick >= 60)
                input.held = tick < 120 ? starfox::input::right : starfox::input::left;
            static_cast<void>(game->tick(input));
            persistent->capture_after_tick(*game);
            const auto& retained = persistent->draw(*game);
            if (tick != 0 && tick != 10 && tick != 30 && tick != 59
                && tick != 90 && tick != 120 && tick != 179) continue;
            auto cold = std::make_unique<starfox::platform_3ds::Frame3ds>(rom, symbols);
            auto warm = std::make_unique<starfox::platform_3ds::Frame3ds>(rom, symbols);
            cold->capture_after_tick(*game);
            warm->capture_after_tick(*game);
            const auto loaded = warm->preload_active_shapes(*game);
            const auto& a = cold->draw(*game);
            const auto& b = warm->draw(*game);
            unsigned pixels = 0, retained_pixels = 0;
            for (std::size_t i = 0; i < a.pixels().size(); ++i) {
                pixels += a.pixels()[i] != b.pixels()[i];
                retained_pixels += retained.pixels()[i] != b.pixels()[i];
            }
            if (argc == 4 && tick == 59) {
                const auto palette = starfox::render::decode_bgr555_palette(game->map().ppu_state().cgram);
                std::ofstream out(std::filesystem::path(argv[3]) / (std::string(scene) + ".ppm"), std::ios::binary);
                out << "P6\n" << a.width() << ' ' << a.height() << "\n255\n";
                for (auto index : a.pixels()) {
                    const char rgb[]{char(palette[index].r), char(palette[index].g), char(palette[index].b)};
                    out.write(rgb, 3);
                }
            }
            std::cout << scene << " tick=" << tick << " preloaded=" << loaded
                << " different_pixels=" << pixels << " retained_pixels=" << retained_pixels << '\n';
            mismatches += pixels != 0 || retained_pixels != 0;
        }
    }
    // Enter GAME OVER through the production level-exit path, preserving the
    // PPU and object state left by gameplay instead of constructing that map.
    auto game = std::make_unique<starfox::simulation::GameSimulation>(
        rom, symbols, "LEVEL1_1", std::span<const std::uint8_t>{}, true);
    game->set_timing_mode(starfox::simulation::TimingMode::unlocked_20_fps);
    game->set_presentation_fps(60U);
    for (unsigned frame = 0; frame < 180; ++frame) {
        game->present_frame();
        if (game->logic_tick_ready()) static_cast<void>(game->tick({}));
    }
    const auto finished = symbols.find("LEVELFINISHED");
    if (finished.empty()) throw std::runtime_error("missing LEVELFINISHED symbol");
    game->map().write_native_word(finished.front(), 10U);
    static_cast<void>(game->tick({}));
    if (game->flow_state() != starfox::simulation::GameFlowState::game_over)
        throw std::runtime_error("level exit did not enter GAME OVER");
    auto persistent = std::make_unique<starfox::platform_3ds::Frame3ds>(rom, symbols);
    static_cast<void>(persistent->preload_catalog(6U * 1024U * 1024U));
    std::vector<std::uint8_t> early_letters;
    for (unsigned frame = 0; frame < 240; ++frame) {
        game->present_frame();
        if (game->logic_tick_ready()) static_cast<void>(game->tick({}));
        persistent->capture_after_tick(*game);
        const auto& retained = persistent->draw(*game);
        if (frame != 81 && frame != 120 && frame != 179 && frame != 239) continue;
        auto cold = std::make_unique<starfox::platform_3ds::Frame3ds>(rom, symbols);
        cold->capture_after_tick(*game);
        const auto& fresh = cold->draw(*game);
        const bool equal = retained.pixels() == fresh.pixels();
        std::cout << "DEATH frame=" << frame << " same_pixels=" << equal << '\n';
        mismatches += !equal;
        if (frame == 81) early_letters = fresh.pixels();
        if (frame == 179 && early_letters == fresh.pixels())
            throw std::runtime_error("GAME OVER remained frozen after its reveal");
        if (argc == 4 && frame == 239) {
            const auto palette = starfox::render::decode_bgr555_palette(game->map().ppu_state().cgram);
            std::ofstream out(std::filesystem::path(argv[3]) / "DEATH.ppm", std::ios::binary);
            out << "P6\n" << fresh.width() << ' ' << fresh.height() << "\n255\n";
            for (auto index : fresh.pixels()) {
                const char rgb[]{char(palette[index].r), char(palette[index].g), char(palette[index].b)};
                out.write(rgb, 3);
            }
        }
    }
    if (mismatches) throw std::runtime_error("precaching changed visible geometry");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
