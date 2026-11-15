#pragma once
#include "audio_3ds.hpp"

#include <cstdio>

// AptAudioPause keeps CSND game audio safe across HOME Menu and sleep transitions. Used by
// the main loop in game_3ds.cpp and by tests/audio_suspend_test.cpp with libctru stubs.

namespace starfox::platform_3ds {

// HOME and sleep transitions must not issue CSND commands: on hardware, a
// command sent from the APT suspend hook leaves the HOME Menu frozen, and so
// does a stop sent just before the transition. main_loop() zeroes the
// looping PCM rings instead, with no sound-service call. After resuming,
// take_resumed() returns true, the caller stops the channels without
// waiting, and the next audio packet starts a fresh ring.
class AptAudioPause {
public:
    explicit AptAudioPause(starfox::platform_3ds::Audio3ds& audio) : audio_{&audio} {
        aptHook(&cookie_, &AptAudioPause::on_event, this);
    }
    ~AptAudioPause() { aptUnhook(&cookie_); }
    AptAudioPause(const AptAudioPause&) = delete;
    AptAudioPause& operator=(const AptAudioPause&) = delete;

    // Use instead of aptMainLoop() wherever game audio may be playing.
    // `quiesce` runs before a pending HOME/POWER transition, while the
    // application is still active, and returns whether the GPU went idle.
    // Returns aptMainLoop()'s result. A pending transition also starts a new
    // step trace in sdmc:/3ds/Starwing/home-trace.txt.
    template<typename Quiesce>
    bool main_loop(Quiesce&& quiesce) {
        if (aptShouldJumpToHome()) {
            trace("w", "home requested");
            audio_->silence_for_suspend();
            trace("a", "rings silenced");
            trace("a", quiesce() ? "gpu idle" : "gpu busy after 500 ms");
            trace("a", "entering transition");
            transition_traced_ = true;
        }
        return aptMainLoop();
    }

    // True once after the application resumes from HOME or sleep (APT
    // restore or wake-up hook), then false until the next resume. Appends
    // the resume step to the trace that main_loop() started, if any.
    [[nodiscard]] bool take_resumed() noexcept {
        const bool value = resumed_;
        resumed_ = false;
        if (value && transition_traced_) {
            trace("a", suspended_ ? "resumed after suspend hook" : "resumed");
            transition_traced_ = false;
        }
        suspended_ = false;
        return value;
    }

private:
    // Writes "<osGetTime() ms> <step>" to the trace file; mode "w" starts a
    // new trace, "a" appends. A file that cannot be opened is skipped.
    static void trace(const char* mode, const char* step) noexcept {
        if (FILE* log = std::fopen("sdmc:/3ds/Starwing/home-trace.txt", mode)) {
            std::fprintf(log, "%llu %s\n",
                static_cast<unsigned long long>(osGetTime()), step);
            std::fclose(log);
        }
    }
    // Runs inside libctru's transition: memory flags only, no service calls.
    static void on_event(APT_HookType type, void* param) {
        auto& self = *static_cast<AptAudioPause*>(param);
        if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP)
            self.suspended_ = true;
        else if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
            self.resumed_ = true;
    }
    starfox::platform_3ds::Audio3ds* audio_;
    aptHookCookie cookie_{};
    volatile bool resumed_{};
    volatile bool suspended_{};
    bool transition_traced_{};
};

} // namespace starfox::platform_3ds
