// Table-driven planar decode and per-frame character sharing must not change
// any rendered pixel. The reference below decodes every pixel bit by bit.
#include "starfox/render/background_renderer.hpp"

#include <cassert>
#include <cstdio>
#include <memory>

namespace {
using starfox::render::Framebuffer;
using starfox::render::TilePriorityPass;
using Ppu = starfox::simulation::SnesPpuState;

std::uint32_t next(std::uint32_t& state) {
    state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
    return state;
}

// SNES tile fetch: 16x16 tiles are four 8x8 characters; flips mirror the
// whole tile. pairs = bitplane pairs (2 = 4bpp, 4 = 8bpp).
std::uint8_t reference_colour(const Ppu& ppu, std::uint16_t character_base,
    std::uint16_t screen_base, std::uint8_t screen_size, bool tile16, unsigned pairs,
    std::uint32_t source_x, std::uint32_t source_y, std::uint16_t* tile_out) {
    const unsigned edge = tile16 ? 16U : 8U;
    const unsigned width_tiles = (screen_size & 1U) ? 64U : 32U;
    const unsigned tx = source_x / edge, ty = source_y / edge;
    const unsigned entry = ((tx >> 5U) + (ty >> 5U) * (width_tiles / 32U)) * 0x400U
        + (ty & 31U) * 32U + (tx & 31U);
    const auto word = ((screen_base + entry) & 0x7fffU) * 2U;
    const auto tile = static_cast<std::uint16_t>(ppu.vram[word] | (ppu.vram[word + 1U] << 8U));
    *tile_out = tile;
    unsigned x = source_x & (edge - 1U), y = source_y & (edge - 1U);
    if (tile & 0x4000U) x = edge - 1U - x;
    if (tile & 0x8000U) y = edge - 1U - y;
    const unsigned character = ((tile & 0x3ffU) + (x >= 8U ? 1U : 0U) + (y >= 8U ? 16U : 0U)) & 0x3ffU;
    x &= 7U; y &= 7U;
    unsigned colour = 0U;
    for (unsigned pair = 0U; pair < pairs; ++pair) {
        const auto base = (character_base * 2U + character * pairs * 16U + pair * 16U + y * 2U) & 0xffffU;
        if (ppu.vram[base] & (0x80U >> x)) colour |= 1U << (pair * 2U);
        if (ppu.vram[(base + 1U) & 0xffffU] & (0x80U >> x)) colour |= 2U << (pair * 2U);
    }
    return static_cast<std::uint8_t>(colour);
}

void randomise(Ppu& ppu, std::uint32_t& state, unsigned trial) {
    for (auto& byte : ppu.vram) byte = static_cast<std::uint8_t>(next(state));
    ppu.background_mode = trial % 2U == 0U ? 3U : 2U;
    ppu.main_screen = 0x13U;
    ppu.mosaic = 0U;
    ppu.bg1_tile_size_16 = (trial & 2U) != 0U;
    ppu.bg2_tile_size_16 = (trial & 1U) != 0U;
    ppu.bg1_screen_size = static_cast<std::uint8_t>(next(state) & 3U);
    ppu.bg2_screen_size = static_cast<std::uint8_t>(next(state) & 3U);
    ppu.bg1_character_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x1000U);
    ppu.bg2_character_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x1000U);
    ppu.bg1_screen_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x0800U);
    ppu.bg2_screen_base = static_cast<std::uint16_t>((next(state) & 7U) * 0x0800U);
    ppu.bg1_scroll_x = static_cast<std::int16_t>(next(state) & 0x3ffU);
    ppu.bg1_scroll_y = static_cast<std::int16_t>(next(state) & 0x3ffU);
    ppu.bg2_vertical_offsets_enabled = false;
    ppu.bg2_horizontal_offsets_enabled = false;
    ppu.bg2_scanline_scroll_enabled = false;
}

// The Mode 3 map-screen sequence: BG2 low, BG1 low, BG2 high, BG1 high.
Framebuffer render_sequence(starfox::render::BackgroundRenderer& renderer, const Ppu& ppu,
    std::int32_t sx, std::int32_t sy) {
    Framebuffer frame(256U, 224U);
    renderer.draw_bg2(ppu, sx, sy, frame, TilePriorityPass::low);
    renderer.draw_bg1(ppu, frame, TilePriorityPass::low);
    renderer.draw_bg2(ppu, sx, sy, frame, TilePriorityPass::high);
    renderer.draw_bg1(ppu, frame, TilePriorityPass::high);
    return frame;
}
} // namespace

int main() {
    std::uint32_t state = 0x2545f491U;
    starfox::render::BackgroundRenderer renderer;
    unsigned decoded = 0U;
    for (unsigned trial = 0U; trial < 64U; ++trial) {
        auto ppu = std::make_unique<Ppu>();
        randomise(*ppu, state, trial);
        const auto sx = static_cast<std::int32_t>(next(state) & 0x3ffU);
        const auto sy = static_cast<std::int32_t>(next(state) & 0x3ffU);
        // Decode against the bit-by-bit reference, BG2 and BG1 separately.
        Framebuffer bg2(256U, 224U), bg1(256U, 224U);
        renderer.draw_bg2(*ppu, sx, sy, bg2);
        renderer.draw_bg1(*ppu, bg1);
        const unsigned width2 = ((ppu->bg2_screen_size & 1U) ? 64U : 32U) * (ppu->bg2_tile_size_16 ? 16U : 8U);
        const unsigned height2 = ((ppu->bg2_screen_size & 2U) ? 64U : 32U) * (ppu->bg2_tile_size_16 ? 16U : 8U);
        const unsigned width1 = ((ppu->bg1_screen_size & 1U) ? 64U : 32U) * (ppu->bg1_tile_size_16 ? 16U : 8U);
        const unsigned height1 = ((ppu->bg1_screen_size & 2U) ? 64U : 32U) * (ppu->bg1_tile_size_16 ? 16U : 8U);
        const unsigned pairs1 = ppu->background_mode == 3U ? 4U : 2U;
        for (unsigned y = 0U; y < 224U; ++y)
            for (unsigned x = 0U; x < 256U; ++x) {
                std::uint16_t tile{};
                auto colour = reference_colour(*ppu, ppu->bg2_character_base, ppu->bg2_screen_base,
                    ppu->bg2_screen_size, ppu->bg2_tile_size_16, 2U,
                    (x + sx) & (width2 - 1U), (y + sy) & (height2 - 1U), &tile);
                assert(bg2.get(x, y) == (colour ? ((tile >> 10U) & 7U) * 16U + colour : 0U));
                colour = reference_colour(*ppu, ppu->bg1_character_base, ppu->bg1_screen_base,
                    ppu->bg1_screen_size, ppu->bg1_tile_size_16, pairs1,
                    (x + ppu->bg1_scroll_x) & (width1 - 1U), (y + ppu->bg1_scroll_y) & (height1 - 1U), &tile);
                const unsigned expected = colour == 0U ? 0U
                    : pairs1 == 4U ? colour : ((tile >> 10U) & 7U) * 16U + colour;
                assert(bg1.get(x, y) == expected);
                decoded += 2U;
            }
        // Sharing characters within one frame changes nothing.
        const auto unshared = render_sequence(renderer, *ppu, sx, sy);
        renderer.begin_frame(*ppu);
        const auto shared = render_sequence(renderer, *ppu, sx, sy);
        // A different PPU object inside the frame must decode its own data.
        auto other = std::make_unique<Ppu>(*ppu);
        for (std::size_t i = 0; i < other->vram.size(); i += 7U) other->vram[i] ^= 0x5aU;
        const auto other_shared = render_sequence(renderer, *other, sx, sy);
        renderer.end_frame();
        assert(shared.pixels() == unshared.pixels());
        assert(other_shared.pixels() == render_sequence(renderer, *other, sx, sy).pixels());
        // A new frame after a VRAM change must not reuse the old characters.
        for (std::size_t i = 0; i < ppu->vram.size(); i += 5U) ppu->vram[i] ^= 0xa5U;
        renderer.begin_frame(*ppu);
        const auto changed = render_sequence(renderer, *ppu, sx, sy);
        renderer.end_frame();
        assert(changed.pixels() == render_sequence(renderer, *ppu, sx, sy).pixels());
    }
    std::printf("character cache: %u decoded pixels match; shared frames identical\n", decoded);
}
