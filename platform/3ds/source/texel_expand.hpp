#pragma once
// Indexed-to-16-bit texel expansion for the PICA staging buffers. Kept free
// of libctru so the exact word paths can be compared on the host.
#include "starfox/render/framebuffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace starfox::platform_3ds {

using Colours16 = std::array<std::uint16_t, 256U>;

inline std::uint32_t load_word(const std::uint8_t* bytes) noexcept {
    std::uint32_t word;
    std::memcpy(&word, bytes, sizeof(word));
    return word;
}

// Two adjacent texels as one aligned store into the 16-bit staging buffer.
inline void store_pair(std::uint16_t* out, std::uint32_t low, std::uint32_t high) noexcept {
    const std::uint32_t pair = low | (high << 16U);
    std::memcpy(out, &pair, sizeof(pair));
}

// Indexed row to 16-bit texels, four pixels per step. A pixel is opaque when
// its mask byte is non-zero; the mask is the write coverage, or the indices
// themselves when index zero is transparent. Most gameplay foreground words
// are empty, so they become two zero stores without palette lookups.
inline void expand_masked_row(const std::uint8_t* pixels, const std::uint8_t* mask,
    const Colours16& colours, std::uint16_t* out, unsigned width) noexcept {
    unsigned x = 0U;
    for (; x + 4U <= width; x += 4U) {
        const auto covered = load_word(mask + x);
        if (covered == 0U) {
            store_pair(out + x, 0U, 0U);
            store_pair(out + x + 2U, 0U, 0U);
            continue;
        }
        const auto indices = load_word(pixels + x);
        const auto texel = [&](unsigned lane) -> std::uint32_t {
            return ((covered >> (lane * 8U)) & 0xffU) != 0U
                ? colours[(indices >> (lane * 8U)) & 0xffU] : 0U;
        };
        store_pair(out + x, texel(0U), texel(1U));
        store_pair(out + x + 2U, texel(2U), texel(3U));
    }
    for (; x < width; ++x) out[x] = mask[x] != 0U ? colours[pixels[x]] : 0U;
}

inline void expand_opaque_row(const std::uint8_t* pixels, const Colours16& colours,
    std::uint16_t* out, unsigned width) noexcept {
    unsigned x = 0U;
    for (; x + 4U <= width; x += 4U) {
        const auto indices = load_word(pixels + x);
        store_pair(out + x, colours[indices & 0xffU], colours[(indices >> 8U) & 0xffU]);
        store_pair(out + x + 2U, colours[(indices >> 16U) & 0xffU], colours[indices >> 24U]);
    }
    for (; x < width; ++x) out[x] = colours[pixels[x]];
}

// Expand a whole frame into a 512-texel-wide staging buffer. A foreground
// uses its coverage when tracked, else palette index zero is transparent.
inline void expand_frame(const render::Framebuffer& frame, const Colours16& colours,
    bool foreground, std::uint16_t* out, std::uint32_t stride) noexcept {
    const auto width = frame.width();
    const auto* pixels = frame.pixels().data();
    const auto coverage = frame.write_coverage();
    const auto* mask = coverage.size() == frame.pixels().size()
        ? coverage.data() : pixels;
    for (std::uint32_t y = 0U; y < frame.height(); ++y) {
        const auto source = static_cast<std::size_t>(y) * width;
        if (foreground)
            expand_masked_row(pixels + source, mask + source, colours, out + y * stride, width);
        else
            expand_opaque_row(pixels + source, colours, out + y * stride, width);
    }
}

} // namespace starfox::platform_3ds
