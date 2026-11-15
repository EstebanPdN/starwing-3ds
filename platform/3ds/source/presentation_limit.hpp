#pragma once
#include <algorithm>
#include <cstdint>

namespace starfox::platform_3ds {
constexpr unsigned valid_fps_limit(unsigned value) noexcept {
    return value==30U || value==60U ? value : 0U;
}
inline constexpr unsigned fps_profile_policy=2U;
constexpr unsigned profile_fps_limit(unsigned default_rate,unsigned stored_rate,
                                      unsigned stored_policy) noexcept {
    // Previous releases saved 60 as Old 3DS's default. Migrate that legacy
    // setting once; only a choice saved under this policy overrides defaults.
    return stored_policy==fps_profile_policy
        &&(stored_rate==0U || stored_rate==30U || stored_rate==60U)
        ?stored_rate:default_rate;
}
// The simulation already consumes a rational 60 Hz phase clock. Select every
// second phase at 30 FPS; variable logic work must not shift the limiter's
// timestamp and accidentally skip a whole extra 16.67 ms display slot.
class PhasePresentationLimit {
public:
    constexpr bool accept(unsigned phases,unsigned requested) noexcept {
        if (!phases) return false;
        const unsigned rate=valid_fps_limit(requested);
        if (!started_ || rate!=rate_) {
            started_=true;rate_=rate;credit_=0;return true;
        }
        if (rate!=30U) return true;
        credit_+=phases;
        if (credit_<2U) return false;
        credit_%=2U;
        return true;
    }
private:
    unsigned rate_{},credit_{};
    bool started_{};
};
// Limits presentation only; late frames never accumulate a burst of draws.
class PresentationLimit {
public:
    explicit constexpr PresentationLimit(std::uint64_t frequency) : frequency_(frequency) {}
    constexpr bool accept(std::uint64_t now,unsigned requested) noexcept {
        const unsigned rate=valid_fps_limit(requested);
        if (!started_ || rate!=rate_) {
            started_=true;rate_=rate;sample_=now;credit_=0;return true;
        }
        const auto elapsed=std::min(now-sample_,frequency_/10U);sample_=now;
        if (!rate) return true;
        credit_=std::min(credit_+elapsed*rate,frequency_*2U);
        if (credit_<frequency_) return false;
        credit_=(credit_-frequency_)%frequency_;
        return true;
    }
private:
    std::uint64_t frequency_,sample_{},credit_{};
    unsigned rate_{};
    bool started_{};
};
} // namespace starfox::platform_3ds
