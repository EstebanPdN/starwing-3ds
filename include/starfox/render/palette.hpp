#pragma once

#include "starfox/render/framebuffer.hpp"
#include "starfox/render/row_workers.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// SNES colour conversion (BGR555 CGRAM words, master brightness) and the
// expansion of indexed framebuffers to RGBA8. Used by the engine, the 3DS
// frontend, host tools and tests.
namespace starfox::render {

struct Rgba8 {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{255};
};

using Palette16 = std::array<Rgba8, 16>;
using Palette256 = std::array<Rgba8, 256>;

// A fixed 16-colour palette for output that has no CGRAM palette.
[[nodiscard]] std::span<const Rgba8, 16> preview_palette() noexcept;
// Opaque RGBA8 from 15-bit BGR555 words; each 5-bit channel is widened by
// repeating its high bits in the low ones.
[[nodiscard]] Palette16 decode_bgr555_palette(
    const std::array<std::uint16_t, 16>& words) noexcept;
[[nodiscard]] Palette256 decode_bgr555_palette(
    const std::array<std::uint16_t, 256>& words) noexcept;
// Scales RGB by brightness / 15 (brightness clamped to 15); alpha is kept.
[[nodiscard]] Palette16 apply_snes_brightness(
    std::span<const Rgba8, 16> palette,
    std::uint8_t brightness) noexcept;
[[nodiscard]] Palette256 apply_snes_brightness(
    std::span<const Rgba8, 256> palette,
    std::uint8_t brightness) noexcept;
// Resizes `destination` to one RGBA8 quad per stored pixel of `source`,
// looked up in `palette` (preview_palette() when omitted). Indices past its
// end use the last entry; an empty palette throws std::invalid_argument.
void expand_rgba(const Framebuffer& source, std::vector<std::uint8_t>& destination);
void expand_rgba(
    const Framebuffer& source,
    std::vector<std::uint8_t>& destination,
    std::span<const Rgba8> palette);
// Large upscale buffers split across the presentation pool by row. Small
// buffers stay serial to avoid a worker hand-off costing more than conversion.
void expand_rgba(
    const Framebuffer& source,
    std::vector<std::uint8_t>& destination,
    std::span<const Rgba8> palette,
    RowWorkers& workers);

} // namespace starfox::render
