#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace starfox::platform_3ds {

// Submission intervals include simulation, rendering and pacing. They do
// not measure LCD scanout, which the asynchronous GPU API does not expose.
// Intentional menus/dumps reset the epoch instead of producing fake stalls.
class PresentationMetrics {
public:
    struct Frame {
        std::uint64_t session_us{};
        std::uint32_t gap_us{}, work_us{}, renderer_us{}, presentation_us{};
        std::uint32_t held{}, phases{}, bg2_quads{}, plan_us{}, plan_wait_us{};
        std::int32_t bg2_x{}, bg2_y{};
        std::uint16_t roll_first{}, roll_last{}, alpha_milli{};
        bool bg2{}, plan_hit{}, paused{};
    };
    struct Interval {
        float gap_p50_ms{}, gap_p95_ms{}, gap_max_ms{};
        unsigned gaps{}, over_25_ms{}, over_50_ms{}, held_or{}, moving_frames{};
    };
    void reset_epoch() noexcept { last_us_ = 0U; }
    void append(Frame frame) noexcept {
        if (last_us_ != 0U)
            frame.gap_us = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(frame.session_us - last_us_, UINT32_MAX));
        last_us_ = frame.session_us;
        frames_[cursor_] = frame;
        cursor_ = (cursor_ + 1U) % frames_.size();
        count_ = std::min(count_ + 1U, frames_.size());
        interval_.held_or |= frame.held;
        // 3DS digital directions plus Circle Pad directions. Shoulder/action
        // buttons remain separately recoverable from the complete held mask.
        if ((frame.held & (0xf0U | 0xf0000000U)) != 0U)
            ++interval_.moving_frames;
        if (frame.gap_us == 0U) return;
        ++interval_.gaps;
        if (frame.gap_us > 25'000U) ++interval_.over_25_ms;
        if (frame.gap_us > 50'000U) ++interval_.over_50_ms;
        interval_.gap_max_ms = std::max(interval_.gap_max_ms,
            static_cast<float>(frame.gap_us) / 1000.0F);
        if (gap_count_ < gaps_.size()) gaps_[gap_count_++] = frame.gap_us;
    }
    [[nodiscard]] Interval finish_interval() noexcept {
        if (gap_count_ != 0U) {
            std::sort(gaps_.begin(), gaps_.begin() + gap_count_);
            interval_.gap_p50_ms = gaps_[(gap_count_ - 1U) / 2U] / 1000.0F;
            interval_.gap_p95_ms = gaps_[(gap_count_ * 95U - 1U) / 100U] / 1000.0F;
        }
        const auto result = interval_;
        interval_ = {};
        gap_count_ = 0U;
        return result;
    }
    [[nodiscard]] std::string csv() const {
        std::string result = "session_us,gap_us,work_us,renderer_us,presentation_us,held,phases,bg2_quads,plan_us,plan_wait_us,bg2_x,bg2_y,roll_first,roll_last,alpha_milli,bg2,plan_hit,paused\n";
        char row[256];
        for (std::size_t n = 0U; n < count_; ++n) {
            const auto& f = frames_[(cursor_ + frames_.size() - count_ + n) % frames_.size()];
            std::snprintf(row, sizeof(row),
                "%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u\n",
                static_cast<unsigned long long>(f.session_us), unsigned(f.gap_us),
                unsigned(f.work_us), unsigned(f.renderer_us), unsigned(f.presentation_us),
                unsigned(f.held), unsigned(f.phases), unsigned(f.bg2_quads),
                unsigned(f.plan_us), unsigned(f.plan_wait_us), int(f.bg2_x), int(f.bg2_y),
                unsigned(f.roll_first), unsigned(f.roll_last), unsigned(f.alpha_milli), unsigned(f.bg2),
                unsigned(f.plan_hit), unsigned(f.paused));
            result += row;
        }
        return result;
    }
private:
    std::array<Frame, 512U> frames_{};
    std::array<std::uint32_t, 256U> gaps_{};
    std::size_t count_{}, cursor_{}, gap_count_{};
    std::uint64_t last_us_{};
    Interval interval_{};
};
} // namespace starfox::platform_3ds
