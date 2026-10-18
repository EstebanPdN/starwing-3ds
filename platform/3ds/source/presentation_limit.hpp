#pragma once
#include <algorithm>
#include <cstdint>

namespace starfox::platform_3ds {
constexpr unsigned valid_fps_limit(unsigned value) noexcept {
    return value==30U || value==60U ? value : 0U;
}
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
