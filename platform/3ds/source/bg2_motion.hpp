#pragma once

#include "starfox/simulation/snes_ppu.hpp"
#include "starfox/timing/fixed_step.hpp"
#include <array>
#include <utility>

namespace starfox::platform_3ds {

// Presentation endpoints only. The native map/PPU remains the authoritative
// source state, just as it does for the interpolated camera and models.
class Bg2Motion {
    struct Snapshot {
        std::uint16_t background{}, character_base{}, screen_base{};
        std::uint8_t mode{}, screen_size{}, main_screen{}, mosaic{};
        bool tile16{}, vertical{}, horizontal{}, scanline{}, tunnel{};
        std::int16_t x{}, y{};
        std::array<std::int16_t, 224U> horizontal_offsets{};
        std::array<std::uint16_t, 32U> vertical_offsets{};

        bool same_layout(const Snapshot& b) const noexcept {
            return background == b.background && mode == b.mode
                && character_base == b.character_base && screen_base == b.screen_base
                && screen_size == b.screen_size && main_screen == b.main_screen
                && mosaic == b.mosaic && tile16 == b.tile16
                && vertical == b.vertical && horizontal == b.horizontal
                && scanline == b.scanline && tunnel == b.tunnel;
        }
    };

    static Snapshot snapshot(const simulation::SnesPpuState& ppu,
        std::int16_t x, std::int16_t y, std::uint16_t background) noexcept {
        Snapshot s{background, ppu.bg2_character_base, ppu.bg2_screen_base,
            ppu.background_mode, ppu.bg2_screen_size, ppu.main_screen, ppu.mosaic,
            ppu.bg2_tile_size_16, ppu.bg2_vertical_offsets_enabled,
            ppu.bg2_horizontal_offsets_enabled, ppu.bg2_scanline_scroll_enabled,
            ppu.tunnel_scene, x, y, ppu.bg2_horizontal_offsets};
        for (unsigned i = 0U; i < s.vertical_offsets.size(); ++i) {
            const auto byte = (0x2fa0U + i) * 2U;
            s.vertical_offsets[i] = static_cast<std::uint16_t>(ppu.vram[byte]
                | (static_cast<unsigned>(ppu.vram[byte + 1U]) << 8U));
        }
        return s;
    }

public:
    void capture(const simulation::SnesPpuState& ppu,
        std::int16_t x, std::int16_t y, std::uint16_t background,
        bool scene_cut) noexcept {
        const auto next = snapshot(ppu, x, y, background);
        previous_ = current_;
        current_ = next;
        if (!ready_ || scene_cut || !previous_.same_layout(current_))
            previous_ = current_;
        ready_ = true;
    }

    [[nodiscard]] bool matches_source(const simulation::SnesPpuState& ppu,
        std::int16_t x, std::int16_t y, std::uint16_t background) const noexcept {
        if (!ready_) return false;
        const auto s = snapshot(ppu, x, y, background);
        return current_.same_layout(s) && current_.x == s.x && current_.y == s.y
            && current_.horizontal_offsets == s.horizontal_offsets
            && current_.vertical_offsets == s.vertical_offsets;
    }

    [[nodiscard]] std::pair<std::int16_t, std::int16_t>
    scroll(double alpha) const noexcept {
        if (alpha >= 1.0) return {current_.x, current_.y};
        return {
            previous_.x == current_.x ? current_.x
                : static_cast<std::int16_t>(timing::interpolate_wrapped_scroll(
                    previous_.x, current_.x, alpha, 0xffffU)),
            previous_.y == current_.y ? current_.y
                : static_cast<std::int16_t>(timing::interpolate_wrapped_scroll(
                    previous_.y, current_.y, alpha, 0x1ffU))};
    }

    void apply(simulation::SnesPpuState& presentation, double alpha) const noexcept {
        if (!ready_ || alpha >= 1.0) return;
        if (current_.horizontal) {
            for (unsigned i = 0U; i < current_.horizontal_offsets.size(); ++i) {
                if (previous_.horizontal_offsets[i] == current_.horizontal_offsets[i]) continue;
                presentation.bg2_horizontal_offsets[i] = static_cast<std::int16_t>(
                    timing::interpolate_wrapped_scroll(previous_.horizontal_offsets[i],
                        current_.horizontal_offsets[i], alpha, 0x1fffU));
            }
        }
        if (current_.vertical) {
            for (unsigned i = 0U; i < current_.vertical_offsets.size(); ++i) {
                const auto before = previous_.vertical_offsets[i];
                const auto after = current_.vertical_offsets[i];
                // Flag changes are discrete table selections. Active DOVOFS
                // scroll follows CALCBGSCROLL's nine-bit wrap, as on PC.
                if (before == after || ((before ^ after) & 0x4000U) != 0U
                    || !(after & 0x4000U))
                    continue;
                const auto value = static_cast<std::uint16_t>((after & 0xe000U)
                    | timing::interpolate_wrapped_scroll(before, after, alpha, 0x1ffU));
                const auto byte = (0x2fa0U + i) * 2U;
                presentation.vram[byte] = static_cast<std::uint8_t>(value);
                presentation.vram[byte + 1U] = static_cast<std::uint8_t>(value >> 8U);
            }
        }
        // IRQ SETBG2VOFS chooses tunnel pages. Its scanline Y values must
        // remain discrete; interpolating them would expose the wrong page.
    }

private:
    Snapshot previous_{}, current_{};
    bool ready_{};
};
} // namespace starfox::platform_3ds
