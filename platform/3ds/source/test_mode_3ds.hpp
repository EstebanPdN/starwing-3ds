#pragma once
// Automatic hardware test tour, started with START+SELECT. It enters each
// level directly, flies with a fixed synthetic controller and asks the game
// loop for one quick dump per measured window. This header holds the state
// machine, the controller script and the text reports. It makes no console
// call, so the host test runs exactly this code.
#include "starfox/input/buttons.hpp"
#include "test_build_label.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace starfox::platform_3ds {

class TestMode3ds {
public:
    // What poll() asks of the game loop: enter the scenario's map, write the
    // window's quick dump, or end the tour.
    enum class Request { none, enter_map, capture, finish };
    // map: the level symbol entered directly, or BOOT. menus: START taps drive
    // the intro and menus instead of the flight script.
    struct Scenario { const char* map; bool menus; };
    // BOOT plays the intro, title and menus with START taps. The rest are
    // the direct entries of the three routes and the two secret stages.
    static constexpr std::array<Scenario, 22> scenarios{{
        {"BOOT", true},
        {"LEVEL1_1", false}, {"LEVEL1_2", false}, {"LEVEL1_3", false},
        {"LEVEL1_4", false}, {"LEVEL1_5", false}, {"LEVEL1_6", false},
        {"LEVEL2_1", false}, {"LEVEL2_2", false}, {"LEVEL2_3", false},
        {"LEVEL2_4", false}, {"LEVEL2_5", false}, {"LEVEL2_6", false},
        {"LEVEL3_1", false}, {"LEVEL3_2", false}, {"LEVEL3_3", false},
        {"LEVEL3_4", false}, {"LEVEL3_5", false}, {"LEVEL3_6", false},
        {"LEVEL3_7", false}, {"LEVEL_BLACKHOLE", false}, {"LEVEL_SPECIAL", false},
    }};
    // Waits after entering a map (settle_ms) and between windows (resettle_ms),
    // the measured window length, and windows per level or menu scenario.
    struct Timing {
        std::uint32_t settle_ms, resettle_ms, window_ms;
        unsigned level_windows, menu_windows;
    };
    // Short: the first 48 s of every level. Long: up to five minutes, or
    // until the level leaves gameplay, so bosses and endings are measured.
    static constexpr Timing short_tour{3000U, 1000U, 15000U, 3U, 2U};
    static constexpr Timing long_tour{3000U, 1000U, 30000U, 10U, 2U};
    static constexpr std::size_t frame_capacity = 2400U;

    [[nodiscard]] bool running() const noexcept {
        return state_ != State::idle && state_ != State::done;
    }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
    [[nodiscard]] bool long_tour_selected() const noexcept { return long_; }
    [[nodiscard]] const std::string& folder() const noexcept { return folder_; }
    [[nodiscard]] unsigned scenario_number() const noexcept {
        return std::min<unsigned>(scenario_ + 1U, scenarios.size());
    }
    [[nodiscard]] unsigned scripted_tick() const noexcept { return tick_; }
    [[nodiscard]] unsigned window_number() const noexcept { return window_ + 1U; }
    [[nodiscard]] unsigned window_count() const noexcept {
        return scenarios[std::min<std::size_t>(scenario_, scenarios.size() - 1U)].menus
            ? timing_.menu_windows : timing_.level_windows;
    }
    [[nodiscard]] unsigned windows_written() const noexcept { return windows_written_; }
    [[nodiscard]] unsigned entry_failures() const noexcept { return entry_failures_; }
    [[nodiscard]] unsigned capture_failures() const noexcept { return capture_failures_; }
    [[nodiscard]] const char* map() const noexcept {
        return scenarios[std::min<std::size_t>(scenario_, scenarios.size() - 1U)].map;
    }

    // Creates sdmc:/3ds/Starwing/prueba/NN (01-99, first free number) and
    // writes the header. Returns false when no folder can be created.
    bool start(const std::string& root, bool long_requested, const std::string& header) {
        if (running()) return false;
        const std::string base = root + "/prueba";
        mkdir(base.c_str(), 0777);
        folder_.clear();
        for (unsigned number = 1U; number <= 99U && folder_.empty(); ++number) {
            char name[8]{};
            std::snprintf(name, sizeof(name), "/%02u", number);
            if (mkdir((base + name).c_str(), 0777) == 0) folder_ = base + name;
        }
        if (folder_.empty()) return false;
        result_path_ = folder_ + "/resultado.txt";
        long_ = long_requested;
        timing_ = long_ ? long_tour : short_tour;
        cancelled_ = false;
        scenario_ = window_ = tick_ = window_tick_ = 0U;
        windows_written_ = entry_failures_ = capture_failures_ = 0U;
        previous_ = 0U;
        frames_.clear();
        frames_.reserve(frame_capacity);
        char line[160]{};
        std::snprintf(line, sizeof(line),
            "recorrido=%s escenarios=%u ventana_s=%u ventanas_por_nivel=%u\n",
            long_ ? "largo" : "corto", static_cast<unsigned>(scenarios.size()),
            static_cast<unsigned>(timing_.window_ms / 1000U), timing_.level_windows);
        append(header + line);
        begin_scenario();
        return true;
    }

    // START during the tour. The loop still receives one finish request.
    void cancel() {
        if (!running() || state_ == State::finishing) return;
        cancelled_ = true;
        finish();
    }

    // Once per loop iteration, before the frame.
    [[nodiscard]] Request poll(std::uint64_t now_ms) noexcept {
        switch (state_) {
        case State::entering:
            if (enter_requested_) return Request::none;
            enter_requested_ = true;
            return Request::enter_map;
        case State::settling:
            if (now_ms - phase_start_ms_ >= settle_ms_) begin_window(now_ms);
            return Request::none;
        case State::measuring:
            if (now_ms - phase_start_ms_ < timing_.window_ms) return Request::none;
            state_ = State::capturing;
            return Request::capture;
        case State::finishing:
            state_ = State::done;
            return Request::finish;
        default:
            return Request::none;
        }
    }

    // The loop's answer to enter_map. Logs the entry; a failure moves on to the
    // next scenario, a success restarts the script and settles.
    void map_entered(bool ok, const std::string& message, std::uint64_t now_ms) {
        if (state_ != State::entering) return;
        char line[200]{};
        std::snprintf(line, sizeof(line), "== %02u/%02u %s entrada=%s%s%s\n",
            scenario_ + 1U, static_cast<unsigned>(scenarios.size()), map(),
            ok ? "ok" : "FALLO", message.empty() ? "" : " ", message.c_str());
        append(line);
        if (!ok) {
            ++entry_failures_;
            next_scenario();
            return;
        }
        tick_ = 0U;
        previous_ = 0U;
        window_ = 0U;
        settle(now_ms, timing_.settle_ms);
    }

    // The controller for one source tick. Levels: slalom, pitch, blaster
    // bursts, a boost, a brake, two rolls, and one SELECT tap in the second
    // window to change the view where the stage allows it. Never START.
    [[nodiscard]] input::TickInput controls() noexcept {
        const unsigned t = tick_++;
        input::ButtonMask held{};
        if (scenarios[scenario_ < scenarios.size() ? scenario_ : 0U].menus) {
            if (t % 40U < 2U) held |= input::start;
        } else {
            const unsigned steer = t % 160U;
            if (steer < 40U) held |= input::left;
            else if (steer >= 80U && steer < 120U) held |= input::right;
            const unsigned pitch = t % 240U;
            if (pitch >= 60U && pitch < 80U) held |= input::up;
            else if (pitch >= 180U && pitch < 200U) held |= input::down;
            if (t % 8U < 4U) held |= input::y;
            const unsigned speed = t % 400U;
            if (speed >= 200U && speed < 230U) held |= input::x;
            else if (speed >= 300U && speed < 320U) held |= input::b;
            const unsigned roll = t % 320U;
            if (roll >= 150U && roll < 160U) held |= input::left_shoulder;
            else if (roll >= 310U) held |= input::right_shoulder;
            if (state_ == State::measuring && window_ == 1U
                && window_tick_ >= 4U && window_tick_ < 6U) held |= input::select;
        }
        if (state_ == State::measuring) ++window_tick_;
        input::TickInput result{};
        result.held = held;
        result.pressed = static_cast<input::ButtonMask>(held & ~previous_);
        result.released = static_cast<input::ButtonMask>(previous_ & ~held);
        previous_ = held;
        return result;
    }

    // One presented loop iteration inside a measured window.
    void note_frame(std::uint64_t now_us, std::uint32_t work_us, bool ticked) noexcept {
        if (state_ != State::measuring || frames_.size() >= frame_capacity) return;
        if (frames_.empty()) window_start_us_ = now_us;
        frames_.push_back({static_cast<std::uint32_t>(now_us - window_start_us_),
            work_us, static_cast<std::uint8_t>(ticked ? 1U : 0U), previous_, tick_});
    }

    // The same per-second values the performance history records.
    void note_second(float fps, float logic_hz) noexcept {
        if (state_ != State::measuring) return;
        fps_min_ = seconds_ == 0U ? fps : std::min(fps_min_, fps);
        fps_sum_ += fps;
        logic_sum_ += logic_hz;
        ++seconds_;
    }

    // After the loop wrote the quick dump for this window (ok) or failed to.
    // level_flow: the game is still in gameplay or training.
    void capture_written(bool ok, const std::string& dump_dir, const char* flow,
        bool level_flow, std::uint64_t now_ms) {
        if (state_ != State::capturing) return;
        const bool menus = scenarios[scenario_].menus;
        const unsigned windows = menus ? timing_.menu_windows : timing_.level_windows;
        std::vector<std::uint32_t> work;
        work.reserve(frames_.size());
        for (const auto& frame : frames_) work.push_back(frame.work_us);
        std::sort(work.begin(), work.end());
        const auto percentile = [&](unsigned percent) {
            return work.empty() ? 0.0
                : work[(work.size() * percent + 99U) / 100U - 1U] / 1000.0;
        };
        const double seconds = seconds_ == 0U ? 1.0 : static_cast<double>(seconds_);
        const auto slash = dump_dir.find_last_of('/');
        const std::string dump_name = !ok ? std::string{"FALLO"}
            : slash == std::string::npos ? dump_dir : dump_dir.substr(slash + 1U);
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "   ventana %u/%u flujo=%s fps=%.1f fps_min=%.1f logica_hz=%.1f "
            "trabajo_p50_ms=%.2f trabajo_p95_ms=%.2f trabajo_p99_ms=%.2f trabajo_max_ms=%.2f "
            "cuadros=%u volcado=%s\n",
            window_ + 1U, windows, flow, fps_sum_ / seconds,
            seconds_ == 0U ? 0.0 : static_cast<double>(fps_min_),
            logic_sum_ / seconds, percentile(50U), percentile(95U), percentile(99U),
            work.empty() ? 0.0 : work.back() / 1000.0,
            static_cast<unsigned>(frames_.size()), dump_name.c_str());
        append(line);
        if (ok) {
            ++windows_written_;
            write_window_files(dump_dir, line);
        } else {
            ++capture_failures_;
        }
        ++window_;
        // The long tour stops a level once it has left gameplay.
        const bool level_over = long_ && !menus && !level_flow;
        if (window_ >= windows || level_over) next_scenario();
        else settle(now_ms, timing_.resettle_ms);
    }

private:
    enum class State { idle, entering, settling, measuring, capturing, finishing, done };
    struct Frame { std::uint32_t at_us, work_us; std::uint8_t ticked; std::uint32_t effective_held{}, scripted_tick{}; };

    void append(const std::string& text) const {
        if (FILE* file = std::fopen(result_path_.c_str(), "a")) {
            std::fputs(text.c_str(), file);
            std::fclose(file);
        }
    }
    void begin_scenario() noexcept {
        state_ = State::entering;
        enter_requested_ = false;
    }
    void settle(std::uint64_t now_ms, std::uint32_t duration_ms) noexcept {
        state_ = State::settling;
        phase_start_ms_ = now_ms;
        settle_ms_ = duration_ms;
    }
    void begin_window(std::uint64_t now_ms) noexcept {
        state_ = State::measuring;
        phase_start_ms_ = now_ms;
        window_tick_ = 0U;
        frames_.clear();
        seconds_ = 0U;
        fps_sum_ = fps_min_ = logic_sum_ = 0.0F;
    }
    void next_scenario() {
        ++scenario_;
        if (scenario_ >= scenarios.size()) finish();
        else begin_scenario();
    }
    void finish() {
        char line[160]{};
        std::snprintf(line, sizeof(line),
            "RECORRIDO %s escenarios=%u ventanas=%u fallos_entrada=%u fallos_volcado=%u\n",
            cancelled_ ? "CANCELADO" : "COMPLETO", scenario_number(),
            windows_written_, entry_failures_, capture_failures_);
        append(line);
        state_ = State::finishing;
    }
    void write_window_files(const std::string& dump_dir, const char* line) const {
        if (FILE* file = std::fopen((dump_dir + "/prueba.txt").c_str(), "w")) {
            std::fprintf(file, "schema=3\nbuild=%.*s\ninput_script=starwing-tour-v1\ninput_clock=source_logic_tick\nescenario=%s\nventana=%u\nrecorrido=%s\n%s",
                static_cast<int>(test_build_label.size()), test_build_label.data(),
                map(), window_ + 1U, long_ ? "largo" : "corto", line);
            std::fclose(file);
        }
        if (FILE* file = std::fopen((dump_dir + "/cuadros.csv").c_str(), "w")) {
            std::fputs("t_us,trabajo_us,tick,effective_held,scripted_tick\n", file);
            for (const auto& frame : frames_)
                std::fprintf(file, "%lu,%lu,%u,%u,%u\n",
                    static_cast<unsigned long>(frame.at_us),
                    static_cast<unsigned long>(frame.work_us),
                    static_cast<unsigned>(frame.ticked),unsigned(frame.effective_held),unsigned(frame.scripted_tick));
            std::fclose(file);
        }
    }

    State state_{State::idle};
    Timing timing_{short_tour};
    bool long_{}, cancelled_{}, enter_requested_{};
    unsigned scenario_{}, window_{}, tick_{}, window_tick_{};
    unsigned windows_written_{}, entry_failures_{}, capture_failures_{};
    std::uint64_t phase_start_ms_{}, window_start_us_{};
    std::uint32_t settle_ms_{};
    input::ButtonMask previous_{};
    std::string folder_, result_path_;
    std::vector<Frame> frames_;
    unsigned seconds_{};
    float fps_sum_{}, fps_min_{}, logic_sum_{};
};

} // namespace starfox::platform_3ds
