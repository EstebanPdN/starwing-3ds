#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include "top_viewport.hpp"
#include "bottom_damage.hpp"
#include "test_build_label.hpp"
// CPU conversion of pictures into the 3DS screens' framebuffer layout: one
// 240-word column per screen column, Y reversed. Used by game_3ds.cpp,
// display_3ds.cpp and the GPU self test; host-tested by
// tests/screen_transfer_test.cpp.

namespace starfox::platform_3ds {
// Fills the 400x240 RGB565 top framebuffer from an indexed picture through
// `view`. `stride` is the source's stored row width and `scale` its draw
// scale; `colours` maps the 256 palette indices to RGB565. Pixels outside the
// viewport are black.
inline void transfer_indexed_screen(const std::uint8_t* source,unsigned stride,
    unsigned scale,const TopViewport& view,const std::uint16_t* colours,
    std::uint16_t* destination) {
    std::array<unsigned,400> columns{};
    std::array<const std::uint8_t*,240> rows{};
    for (unsigned x=view.x;x<view.x+view.width;++x)
        columns[x]=view.sample_x(x)*scale;
    for (unsigned y=view.y;y<view.y+view.height;++y)
        rows[y]=source+std::size_t(view.sample_y(y))*scale*stride;
    for (unsigned by=0;by<240;by+=8) for (unsigned bx=0;bx<400;bx+=8)
        for (unsigned x=bx;x<bx+8;++x) {
            auto* out=destination+std::size_t(x)*240+232-by;
            if (x<view.x || x>=view.x+view.width) {
                for (unsigned n=0;n<8;++n) out[n]=0;
            } else {
                const auto sx=columns[x];
                for (unsigned n=0;n<8;++n) {
                    const auto* row=rows[by+7-n];
                    out[n]=row?colours[row[sx]]:0;
                }
            }
        }
}
// channel * brightness / 15 for every channel value and brightness 0..15:
// three lookups per faded pixel instead of three divisions.
inline constexpr auto brightness_channels = [] {
    std::array<std::array<std::uint8_t, 256>, 16> table{};
    for (unsigned brightness = 0U; brightness < 16U; ++brightness)
        for (unsigned channel = 0U; channel < 256U; ++channel)
            table[brightness][channel] =
                static_cast<std::uint8_t>(channel * brightness / 15U);
    return table;
}();
// One RGBA menu pixel as the bottom framebuffer shows it at a brightness of
// 0..15. Full brightness passes the pixel through, alpha byte included.
constexpr std::uint32_t framebuffer_rgba(std::uint32_t c, unsigned brightness) noexcept {
    if (brightness != 15U) {
        const auto& scaled = brightness_channels[brightness & 15U];
        c = (std::uint32_t(scaled[c >> 24U]) << 24U)
            | (std::uint32_t(scaled[(c >> 16U) & 255U]) << 16U)
            | (std::uint32_t(scaled[(c >> 8U) & 255U]) << 8U) | 255U;
    }
    return c;
}
// Native framebuffer columns are contiguous, with Y reversed. Tile the source
// reads and write consecutive destination words instead of scattering every
// store by 960 bytes into the GPU-visible framebuffer.
inline void transfer_rgba_screen(const std::uint32_t* source,
    std::uint32_t* destination, int width, unsigned brightness) {
    for (int by = 0; by < 240; by += 8) {
        for (int bx = 0; bx < width; bx += 8) {
            for (int x = bx; x < bx + 8 && x < width; ++x) {
                auto* out = destination + std::size_t(x) * 240U + 232U - by;
                for (int n = 0; n < 8; ++n)
                    out[n] = framebuffer_rgba(
                        source[std::size_t(by + 7 - n) * width + x], brightness);
            }
        }
    }
}
// The same conversion for one rectangle of the 320-pixel-wide bottom image:
// every pixel of `area` lands where transfer_rgba_screen() puts it.
inline void transfer_rgba_rect(const std::uint32_t* source,
    std::uint32_t* destination, unsigned brightness, const ScreenRect& area) {
    for (int by = area.top; by < area.bottom; by += 8) {
        const int rows = std::min(8, area.bottom - by);
        for (int x = area.left; x < area.right; ++x) {
            auto* out = destination + std::size_t(x) * 240U
                + std::size_t(240 - by - rows);
            const auto* in = source
                + std::size_t(by + rows - 1) * bottom_screen_width + x;
            for (int n = 0; n < rows; ++n, in -= bottom_screen_width)
                out[n] = framebuffer_rgba(*in, brightness);
        }
    }
}
// The framebuffer words [first, last) a rectangle of the image occupies.
struct FramebufferWords {
    std::size_t first{}, last{};
    [[nodiscard]] constexpr bool empty() const noexcept { return first >= last; }
    constexpr void include(const ScreenRect& area) noexcept {
        if (area.empty()) return;
        const auto begin = std::size_t(area.left) * 240U + std::size_t(240 - area.bottom);
        const auto end = std::size_t(area.right - 1) * 240U + std::size_t(240 - area.top);
        if (empty()) { first = begin; last = end; }
        else { first = std::min(first, begin); last = std::max(last, end); }
    }
};
// Fills the bottom screen's framebuffers from the retained RGBA image, and
// knows how far each of them is behind it. libctru double-buffers the screen:
// the buffer being filled was last filled two presentations ago, so it needs
// everything the image's owner rewrote since then, not only the latest
// change. A buffer filled at another brightness, or one that held anything
// but this image, is converted whole. No libctru here: the caller flushes the
// returned words and swaps.
class BottomScreenTransfer {
public:
    // Before each presentation. `changed` covers what the owner of the image
    // rewrote since the previous presentation that passed one; null when the
    // image does not continue the previous one (another screen's picture).
    constexpr void note(const ScreenDamage* changed) noexcept {
        continues_ = changed != nullptr;
        for (auto& buffer : buffers_) {
            if (changed == nullptr || changed->everything()) buffer.known = false;
            else buffer.pending.add(*changed);
        }
    }
    // Neither framebuffer is known to hold the image.
    constexpr void forget() noexcept {
        for (auto& buffer : buffers_) buffer.known = false;
        presentations_ = 0U;
    }
    // Make `framebuffer`, the buffer about to be presented, show `image` at
    // `brightness`. Returns the words written, for the data-cache flush.
    inline FramebufferWords transfer(const std::uint32_t* image,
        std::uint32_t* framebuffer, unsigned brightness) noexcept {
        brightness = std::min(brightness, 15U);
        // A double-buffered screen alternates two addresses and a single
        // buffer repeats one. Any other order means another writer presented
        // this screen in between; start again from nothing.
        if (presentations_ >= 2U && framebuffer != before_previous_) forget();
        Buffer* buffer = nullptr;
        for (auto& candidate : buffers_)
            if (candidate.address == framebuffer) buffer = &candidate;
        if (buffer == nullptr) {
            buffer = buffers_[0].address == previous_ ? &buffers_[1] : &buffers_[0];
            *buffer = Buffer{};
            buffer->address = framebuffer;
        }
        FramebufferWords written{};
        if (!buffer->known || buffer->brightness != brightness
            || buffer->pending.everything()) {
            transfer_rgba_screen(image, framebuffer, bottom_screen_width, brightness);
            draw_test_build_label(framebuffer);
            written = {0U, std::size_t(bottom_screen_width) * bottom_screen_height};
        } else {
            constexpr ScreenRect label{
                static_cast<std::int16_t>(test_build_label_area.left),
                static_cast<std::int16_t>(test_build_label_area.top),
                static_cast<std::int16_t>(test_build_label_area.right),
                static_cast<std::int16_t>(test_build_label_area.bottom)};
            bool over_label = false;
            for (const auto& area : buffer->pending) {
                transfer_rgba_rect(image, framebuffer, brightness, area);
                written.include(area);
                over_label = over_label || area.intersects(label);
            }
            if (over_label) {
                draw_test_build_label(framebuffer);
                written.include(label);
            }
        }
        buffer->pending.clear();
        buffer->known = continues_;
        buffer->brightness = static_cast<std::uint8_t>(brightness);
        before_previous_ = previous_;
        previous_ = framebuffer;
        if (presentations_ < 2U) ++presentations_;
        return written;
    }

private:
    struct Buffer {
        const std::uint32_t* address{};
        ScreenDamage pending{};
        std::uint8_t brightness{};
        bool known{};
    };
    std::array<Buffer, 2U> buffers_{};
    const std::uint32_t* previous_{};
    const std::uint32_t* before_previous_{};
    unsigned presentations_{};
    bool continues_{};
};
// A row-major RGBA8 picture, `width` x 240 (red in the high byte), into an
// RGB565 framebuffer at brightness 0..15; game_3ds.cpp uses it for the top screen.
inline void transfer_rgb565_screen(const std::uint32_t* source,
    std::uint16_t* destination, int width, unsigned brightness) {
    for (int by=0;by<240;by+=8) for (int bx=0;bx<width;bx+=8) {
        for (int x=bx;x<bx+8&&x<width;++x) {
            auto* out=destination+std::size_t(x)*240U+232U-by;
            for (int n=0;n<8;++n) {
                const auto c=source[std::size_t(by+7-n)*width+x];
                const auto r=((c>>24U)&255U)*brightness/15U;
                const auto g=((c>>16U)&255U)*brightness/15U;
                const auto b=((c>>8U)&255U)*brightness/15U;
                out[n]=static_cast<std::uint16_t>((r>>3U)<<11U|(g>>2U)<<5U|(b>>3U));
            }
        }
    }
}
}
