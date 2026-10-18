#pragma once

#include "bg2_atlas_cache.hpp"

namespace starfox::platform_3ds {

// The complete BG2 image depends on map words and referenced character
// bytes. Roll/HDMA tables can change VRAM without changing this image.
class Bg2MapSourceCache {
public:
    void reset() noexcept { valid_ = false; }
    [[nodiscard]] bool all_opaque() const noexcept { return valid_ && all_opaque_; }
    [[nodiscard]] bool matches(const simulation::SnesPpuState& ppu) const noexcept {
        if (!valid_ || ppu.bg2_tile_size_16 || ppu.bg2_character_base != character_base_
            || ppu.bg2_screen_base != screen_base_ || ppu.bg2_screen_size != screen_size_)
            return false;
        const auto address = (std::uint32_t(screen_base_) * 2U) & 65535U;
        const auto first = std::min<std::size_t>(bytes_, 65536U - address);
        return std::memcmp(words_.data(), ppu.vram.data() + address, first) == 0
            && (first == bytes_ || std::memcmp(words_.data() + first,
                    ppu.vram.data(), bytes_ - first) == 0)
            && characters_.matches(ppu);
    }
    void remember(const simulation::SnesPpuState& ppu) noexcept {
        valid_ = false;
        if (ppu.bg2_tile_size_16) return;
        character_base_ = ppu.bg2_character_base;
        screen_base_ = ppu.bg2_screen_base;
        screen_size_ = ppu.bg2_screen_size;
        bytes_ = ((screen_size_ & 1U) ? 64U : 32U)
            * ((screen_size_ & 2U) ? 64U : 32U) * 2U;
        const auto address = (std::uint32_t(screen_base_) * 2U) & 65535U;
        const auto first = std::min<std::size_t>(bytes_, 65536U - address);
        std::memcpy(words_.data(), ppu.vram.data() + address, first);
        if (first != bytes_) std::memcpy(words_.data() + first, ppu.vram.data(), bytes_ - first);
        characters_.reset();
        std::array<std::uint64_t, 16U> examined{};
        all_opaque_ = true;
        for (std::size_t i = 0U; i < bytes_; i += 2U) {
            const auto character = static_cast<std::uint16_t>(words_[i]
                | (std::uint16_t(words_[i + 1U]) << 8U)) & 1023U;
            const auto bit = std::uint64_t{1} << (character & 63U);
            auto& used = examined[character >> 6U];
            if ((used & bit) != 0U) continue;
            used |= bit;
            characters_.remember(ppu, character);
            // An opaque map needs ground-carry corrections only after a
            // source-Y wrap. Inspect each referenced character once; this
            // certificate shares the map's byte-exact invalidation rules.
            if (!all_opaque_) continue;
            const auto base = (std::uint32_t(character_base_) * 2U
                + character * 32U) & 65535U;
            for (unsigned row = 0U; row < 8U; ++row) {
                const auto at = (base + row * 2U) & 65535U;
                if ((ppu.vram[at] | ppu.vram[(at + 1U) & 65535U]
                        | ppu.vram[(at + 16U) & 65535U]
                        | ppu.vram[(at + 17U) & 65535U]) != 255U) {
                    all_opaque_ = false;
                    break;
                }
            }
        }
        valid_ = true;
    }
private:
    Bg2AtlasSourceCache characters_{};
    std::array<std::uint8_t, 8192U> words_{};
    std::size_t bytes_{};
    std::uint16_t character_base_{}, screen_base_{};
    std::uint8_t screen_size_{};
    bool valid_{};
    bool all_opaque_{};
};
} // namespace starfox::platform_3ds
