#pragma once
#include "audio_3ds.hpp"

#include <cstdio>

namespace starfox::platform_3ds {

// HOME and sleep transitions must not issue CSND commands: commands sent
// from the APT suspend hook left the HOME Menu frozen on hardware. Silence
// the looping PCM ring in ordinary context before the transition, and let
// the first source tick after resuming start a fresh ring.
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
    template<typename Quiesce>
    bool main_loop(Quiesce&& quiesce) {
        if (aptShouldJumpToHome()) {
            trace("w", "home requested");
            audio_->stop_playback();
            trace("a", "audio stopped");
            trace("a", quiesce() ? "gpu idle" : "gpu busy after 500 ms");
            trace("a", "entering transition");
            transition_traced_ = true;
        }
        return aptMainLoop();
    }

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
