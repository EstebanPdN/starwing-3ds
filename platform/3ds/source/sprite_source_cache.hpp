#pragma once

#include "starfox/render/sprite_renderer.hpp"

#include <array>
#include <cstring>

namespace starfox::platform_3ds {

// An indexed OBJ layer depends on selected OAM records and their character
// bytes, not on palette colours, BG2 scroll tables or unrelated sprite groups.
// Keep exact bytes rather than a hash. Fixed storage covers all 2048 possible
// 32-byte VRAM characters, including wrapped names and 64x64 objects.
class SpriteSourceCache {
public:
    enum class Selection { foreground, status_hud, bottom_hud };
    explicit SpriteSourceCache(Selection selection) : selection_(selection) {}
    [[nodiscard]] std::size_t character_count() const noexcept { return character_count_; }

    [[nodiscard]] bool matches(const simulation::SnesPpuState& ppu,
        std::uint64_t vram_revision) noexcept {
        if (!valid_ || object_select_ != ppu.object_select
            || objects_enabled_ != ((ppu.main_screen & 0x10U) != 0U)) return false;
        if (!objects_enabled_ && selection_ != Selection::bottom_hud) return true;
        for (std::size_t object = 0U; object < 128U; ++object)
            if (records_[object] != record(ppu, object)) return false;
        if (vram_revision_ != vram_revision || selection_ == Selection::bottom_hud) {
            for (std::size_t i = 0U; i < character_count_; ++i) {
                const auto address = static_cast<std::size_t>(characters_[i]) * 32U;
                if (std::memcmp(bytes_.data() + address,
                        ppu.vram.data() + address, 32U) != 0) return false;
            }
            vram_revision_ = vram_revision;
        }
        return true;
    }

    void capture(const simulation::SnesPpuState& ppu,
        std::uint64_t vram_revision) noexcept {
        object_select_ = ppu.object_select;
        objects_enabled_ = (ppu.main_screen & 0x10U) != 0U;
        vram_revision_ = vram_revision;
        character_count_ = 0U;
        valid_ = true;
        if (!objects_enabled_ && selection_ != Selection::bottom_hud) return;
        std::array<std::uint8_t, 2048U> used{};
        constexpr std::array<std::array<unsigned, 2U>, 6U> sizes{{
            {{8, 16}}, {{8, 32}}, {{8, 64}}, {{16, 32}}, {{16, 64}}, {{32, 64}}}};
        const auto selection = std::min<std::size_t>((object_select_ >> 5U) & 7U, 5U);
        const auto base = static_cast<unsigned>(object_select_ & 7U) * 0x4000U;
        const auto gap = static_cast<unsigned>(((object_select_ >> 3U) & 3U) + 1U) * 0x2000U;
        for (std::size_t object = 0U; object < 128U; ++object) {
            const auto value = records_[object] = record(ppu, object);
            if (value == Record{}) continue;
            const auto tile = static_cast<unsigned>(value[2]);
            const auto tile_base = base + ((value[3] & 1U) != 0U ? gap : 0U);
            const auto tiles = sizes[selection][(value[4] >> 1U) & 1U] / 8U;
            for (unsigned y = 0U; y < tiles; ++y)
                for (unsigned x = 0U; x < tiles; ++x) {
                    const auto address = (tile_base + (tile + y * 16U + x) * 32U) & 0xffffU;
                    const auto character = address / 32U;
                    if (used[character] != 0U) continue;
                    used[character] = 1U;
                    characters_[character_count_++] = static_cast<std::uint16_t>(character);
                    std::memcpy(bytes_.data() + address, ppu.vram.data() + address, 32U);
                }
        }
    }

private:
    using Record = std::array<std::uint8_t, 5U>;
    [[nodiscard]] Record record(const simulation::SnesPpuState& ppu,
        std::size_t object) const noexcept {
        const auto low = object * 4U;
        const auto* data = ppu.oam.data() + low;
        const auto tile = static_cast<std::uint16_t>(data[2] | ((data[3] & 1U) << 8U));
        const auto is_life=[&](std::size_t n) {
            return ppu.oam[n*4+2]==189U && !(ppu.oam[n*4+3]&1U);
        };
        // The bottom panel reads bomb icons even outside the visible OBJ
        // area and the positions of the two records following a life icon.
        const bool bottom_extra=selection_==Selection::bottom_hud
            && (tile==188 || tile==189 || (object>0 && is_life(object-1))
                || (object>1 && is_life(object-2)));
        if (!bottom_extra && ((data[0] == 0U && data[1] == 0U && data[2] == 0U && data[3] == 0U)
            || (data[0] == 0xf8U && data[1] == 0xf8U)
            || (selection_==Selection::bottom_hud && !(ppu.main_screen&0x10U)))) return {};
        const auto high = static_cast<std::uint8_t>(
            (ppu.oam[512U + object / 4U] >> ((object & 3U) * 2U)) & 3U);
        auto x = static_cast<std::int32_t>(data[0])
            | static_cast<std::int32_t>((high & 1U) << 8U);
        if (x >= 256) x -= 512;
        const auto element = render::object_hud_element(tile, data[1], x);
        const bool selected = selection_ == Selection::foreground ? !element
            : element && *element != render::HudElement::comms;
        return selected || bottom_extra ? Record{data[0], data[1], data[2], data[3], high} : Record{};
    }

    Selection selection_;
    std::array<Record, 128U> records_{};
    std::array<std::uint8_t, 65536U> bytes_{};
    std::array<std::uint16_t, 2048U> characters_{};
    std::size_t character_count_{};
    std::uint64_t vram_revision_{};
    std::uint8_t object_select_{};
    bool objects_enabled_{};
    bool valid_{};
};

} // namespace starfox::platform_3ds
