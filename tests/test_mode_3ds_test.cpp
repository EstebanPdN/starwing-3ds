#include "test_mode_3ds.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using starfox::platform_3ds::TestMode3ds;
namespace input = starfox::input;
namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

unsigned count(const std::string& text, const std::string& needle) {
    unsigned found = 0;
    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) ++found;
    return found;
}

struct Outcome {
    unsigned entries{}, captures{};
    bool finished{};
    std::vector<std::string> maps, dumps;
    double simulated_minutes{};
};

struct Options {
    bool long_tour{};
    std::string failing_map;          // entry rejected for this map
    bool stay_in_level{true};         // flow reported at each capture
    unsigned failing_capture{};       // 1-based capture that cannot be written
    unsigned cancel_at_frame{};       // 0 = never
};

// Drives the tour as the game loop does: 60 loop iterations and 20 source
// ticks a second, one-second statistics, and a two-second pause per dump.
Outcome drive(TestMode3ds& test, const fs::path& root, const Options& options) {
    Outcome outcome;
    std::uint64_t now_us = 5'000'000U;
    input::ButtonMask previous{};
    unsigned frame = 0;
    for (; frame < 2'000'000U && !outcome.finished; ++frame) {
        now_us += 16'667U;
        const auto now_ms = now_us / 1000U;
        if (options.cancel_at_frame != 0U && frame == options.cancel_at_frame) test.cancel();
        bool capture = false;
        switch (test.poll(now_ms)) {
        case TestMode3ds::Request::enter_map: {
            ++outcome.entries;
            outcome.maps.emplace_back(test.map());
            const bool ok = options.failing_map != test.map();
            test.map_entered(ok, ok ? "" : "sin memoria", now_ms);
            // A new simulation starts from a released controller. After a
            // rejected entry the previous one keeps running.
            if (ok) previous = 0U;
            break;
        }
        case TestMode3ds::Request::capture: capture = true; break;
        case TestMode3ds::Request::finish: outcome.finished = true; break;
        default: break;
        }
        if (outcome.finished) break;
        assert(test.running());
        const bool ticked = frame % 3U == 0U;
        if (ticked) {
            const std::string map = test.map();
            const auto controls = test.controls();
            assert(controls.pressed == (controls.held & ~previous));
            assert(controls.released == (previous & ~controls.held));
            previous = controls.held;
            if (map == "BOOT") assert((controls.held & ~input::start) == 0U);
            else assert((controls.held & input::start) == 0U);
            if ((controls.held & input::select) != 0U)
                assert(map != "BOOT" && test.window_number() == 2U);
        }
        test.note_frame(now_us, 9000U + frame % 7U * 500U, ticked);
        if (frame % 60U == 59U) test.note_second(59.5F, 20.0F);
        if (capture) {
            ++outcome.captures;
            const bool ok = outcome.captures != options.failing_capture;
            char name[40]{};
            std::snprintf(name, sizeof(name), "%03u-dump-quick", outcome.captures);
            const auto dump = root / "dumps" / name;
            if (ok) {
                fs::create_directories(dump);
                outcome.dumps.push_back(dump.string());
            }
            now_us += 2'000'000U;
            test.capture_written(ok, ok ? dump.string() : std::string{}, "GAMEPLAY",
                options.stay_in_level, now_us / 1000U);
        }
    }
    assert(outcome.finished && !test.running());
    outcome.simulated_minutes = (now_us - 5'000'000U) / 60e6;
    return outcome;
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path()
        / ("starwing-test-mode-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string header = "cabecera de prueba\n";
    const auto total = static_cast<unsigned>(TestMode3ds::scenarios.size());
    assert(total == 22U);

    // Short tour: menus twice, every level three times.
    {
        TestMode3ds test;
        assert(!test.running());
        assert(test.poll(0U) == TestMode3ds::Request::none);
        assert(test.start(root.string(), false, header));
        assert(test.running() && !test.long_tour_selected());
        assert(!test.start(root.string(), false, header));
        assert(test.folder() == (root / "prueba" / "01").string());
        const auto run = drive(test, root, {});
        assert(run.entries == total && run.maps.front() == "BOOT");
        assert(run.maps[1] == "LEVEL1_1" && run.maps.back() == "LEVEL_SPECIAL");
        assert(run.captures == 2U + 21U * 3U && test.windows_written() == run.captures);
        assert(!test.cancelled() && test.entry_failures() == 0U && test.capture_failures() == 0U);
        const auto result = read_text(root / "prueba" / "01" / "resultado.txt");
        assert(result.rfind(header, 0) == 0);
        assert(count(result, "recorrido=corto escenarios=22 ventana_s=15 ventanas_por_nivel=3\n") == 1U);
        assert(count(result, "\n== ") == total && count(result, "entrada=ok") == total);
        assert(count(result, "   ventana ") == run.captures);
        assert(count(result, "== 01/22 BOOT entrada=ok\n   ventana 1/2 flujo=GAMEPLAY fps=59.5 fps_min=59.5 logica_hz=20.0 ") == 1U);
        assert(count(result, "RECORRIDO COMPLETO escenarios=22 ventanas=65 fallos_entrada=0 fallos_volcado=0\n") == 1U);
        for (const auto& dump : run.dumps) {
            const auto note = read_text(fs::path{dump} / "prueba.txt");
            assert(count(note, "escenario=") == 1U && count(note, "recorrido=corto") == 1U);
            const auto frames = read_text(fs::path{dump} / "cuadros.csv");
            assert(frames.rfind("t_us,trabajo_us,tick,effective_held,scripted_tick\n0,", 0) == 0);
            const auto rows = count(frames, "\n") - 1U;
            assert(rows >= 880U && rows <= 905U);
        }
        std::printf("short tour: %u windows, %.1f simulated minutes\n",
            run.captures, run.simulated_minutes);
    }

    // The next tour takes the next free folder.
    {
        TestMode3ds test;
        assert(test.start(root.string(), true, header));
        assert(test.folder() == (root / "prueba" / "02").string() && test.long_tour_selected());
        // Long tour, each level already over at its first dump: one window each.
        Options options;
        options.long_tour = true;
        options.stay_in_level = false;
        const auto run = drive(test, root, options);
        assert(run.entries == total && run.captures == 2U + 21U);
        const auto result = read_text(root / "prueba" / "02" / "resultado.txt");
        assert(count(result, "recorrido=largo escenarios=22 ventana_s=30 ventanas_por_nivel=10\n") == 1U);
        assert(count(result, "   ventana 1/10 ") == 21U && count(result, "   ventana 2/10 ") == 0U);
        const auto rows = count(read_text(fs::path{run.dumps.back()} / "cuadros.csv"), "\n") - 1U;
        assert(rows >= 1780U && rows <= 1805U);
    }

    // Long tour that stays in gameplay: ten windows per level.
    {
        TestMode3ds test;
        assert(test.start(root.string(), true, header));
        Options options;
        options.long_tour = true;
        const auto run = drive(test, root, options);
        assert(run.captures == 2U + 21U * 10U);
        std::printf("long tour: %u windows, %.1f simulated minutes\n",
            run.captures, run.simulated_minutes);
    }

    // A level that cannot be entered is recorded and skipped.
    {
        TestMode3ds test;
        assert(test.start(root.string(), false, header));
        Options options;
        options.failing_map = "LEVEL2_3";
        options.failing_capture = 4U;
        const auto run = drive(test, root, options);
        assert(run.entries == total && run.captures == 2U + 20U * 3U);
        assert(test.entry_failures() == 1U && test.capture_failures() == 1U);
        assert(test.windows_written() == run.captures - 1U);
        const auto result = read_text(fs::path{test.folder()} / "resultado.txt");
        assert(count(result, "LEVEL2_3 entrada=FALLO sin memoria\n== ") == 1U);
        assert(count(result, "volcado=FALLO\n") == 1U);
        assert(count(result, "fallos_entrada=1 fallos_volcado=1\n") == 1U);
    }

    // START cancels: one finish request, a closing line, nothing afterwards.
    {
        TestMode3ds test;
        assert(test.start(root.string(), false, header));
        Options options;
        options.cancel_at_frame = 60U * 70U;
        const auto run = drive(test, root, options);
        assert(test.cancelled() && run.entries < total);
        const auto result = read_text(fs::path{test.folder()} / "resultado.txt");
        assert(count(result, "RECORRIDO CANCELADO escenarios=") == 1U);
        assert(count(result, "RECORRIDO COMPLETO") == 0U);
        test.cancel();
        assert(test.poll(0U) == TestMode3ds::Request::none && !test.running());
    }

    // No folder can be created under a regular file: the tour does not start.
    {
        const auto file = root / "archivo";
        std::ofstream{file} << "x";
        TestMode3ds test;
        assert(!test.start(file.string(), false, header) && !test.running());
    }

    fs::remove_all(root);
    std::puts("PASS: test tour order, windows, controller script, reports, failures and cancel");
}
