#pragma once
// Which pixels of the retained 320x240 bottom image changed. The image's
// owner names the rectangles it rewrote; presentation converts and flushes
// those instead of the whole screen. Free of libctru for host tests.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace starfox::platform_3ds {

inline constexpr int bottom_screen_width = 320;
inline constexpr int bottom_screen_height = 240;

// Half-open rectangle of bottom-screen pixels.
struct ScreenRect {
    std::int16_t left{}, top{}, right{}, bottom{};
    [[nodiscard]] constexpr bool empty() const noexcept {
        return left >= right || top >= bottom;
    }
    [[nodiscard]] constexpr bool contains(const ScreenRect& other) const noexcept {
        return left <= other.left && top <= other.top
            && right >= other.right && bottom >= other.bottom;
    }
    [[nodiscard]] constexpr bool intersects(const ScreenRect& other) const noexcept {
        return !empty() && !other.empty() && left < other.right && other.left < right
            && top < other.bottom && other.top < bottom;
    }
    friend constexpr bool operator==(const ScreenRect&, const ScreenRect&) = default;
};

// The part of x, y, width, height that is on the screen.
[[nodiscard]] constexpr ScreenRect screen_rect(int x, int y, int width, int height) noexcept {
    const int left = std::clamp(x, 0, bottom_screen_width);
    const int right = std::clamp(x + std::max(width, 0), 0, bottom_screen_width);
    const int top = std::clamp(y, 0, bottom_screen_height);
    const int bottom = std::clamp(y + std::max(height, 0), 0, bottom_screen_height);
    return {static_cast<std::int16_t>(left), static_cast<std::int16_t>(top),
        static_cast<std::int16_t>(right), static_cast<std::int16_t>(bottom)};
}

[[nodiscard]] constexpr ScreenRect bounding_rect(const ScreenRect& a,
    const ScreenRect& b) noexcept {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.left, b.left), std::min(a.top, b.top),
        std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

// A bounded cover of the changed pixels: a few rectangles, or everything.
// Adding never loses a pixel; when the list is full it widens to one box.
class ScreenDamage {
public:
    static constexpr std::size_t capacity = 8U;

    constexpr void clear() noexcept { count_ = 0U; everything_ = false; }
    constexpr void add_everything() noexcept { count_ = 0U; everything_ = true; }
    constexpr void add(ScreenRect area) noexcept {
        if (everything_) return;
        area = screen_rect(area.left, area.top, area.right - area.left,
            area.bottom - area.top);
        if (area.empty()) return;
        std::size_t kept = 0U;
        for (std::size_t i = 0U; i < count_; ++i) {
            if (areas_[i].contains(area)) return;
            if (!area.contains(areas_[i])) areas_[kept++] = areas_[i];
        }
        count_ = kept;
        if (count_ == capacity) {
            for (std::size_t i = 0U; i < count_; ++i)
                area = bounding_rect(area, areas_[i]);
            count_ = 0U;
        }
        areas_[count_++] = area;
    }
    constexpr void add(const ScreenDamage& other) noexcept {
        if (other.everything_) add_everything();
        else for (std::size_t i = 0U; i < other.count_; ++i) add(other.areas_[i]);
    }
    [[nodiscard]] constexpr bool everything() const noexcept { return everything_; }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return !everything_ && count_ == 0U;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
    [[nodiscard]] constexpr const ScreenRect& operator[](std::size_t index) const noexcept {
        return areas_[index];
    }
    [[nodiscard]] constexpr const ScreenRect* begin() const noexcept { return areas_.data(); }
    [[nodiscard]] constexpr const ScreenRect* end() const noexcept {
        return areas_.data() + count_;
    }

private:
    std::array<ScreenRect, capacity> areas_{};
    std::size_t count_{};
    bool everything_{};
};

} // namespace starfox::platform_3ds
