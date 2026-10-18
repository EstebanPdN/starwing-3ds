#pragma once

#include "starfox/simulation/snes_ppu.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace starfox::platform_3ds {

// Only decoded characters own atlas texels. Tile maps, HDMA and unused
// characters may change VRAM without changing any of those texels.
// Fixed storage covers all 1024 characters and cannot grow across scenes.
class Bg2AtlasSourceCache {
public:
    void reset() noexcept { used_.fill(0U); }

    void remember(const simulation::SnesPpuState& ppu,
        std::uint16_t character) noexcept {
        character &= 1023U;
        const auto address = (std::uint32_t(ppu.bg2_character_base) * 2U
            + std::uint32_t(character) * 32U) & 65535U;
        const auto first = std::min<std::uint32_t>(32U, 65536U - address);
        auto* destination = bytes_.data() + character * 32U;
        std::memcpy(destination, ppu.vram.data() + address, first);
        if (first != 32U)
            std::memcpy(destination + first, ppu.vram.data(), 32U - first);
        used_[character / 64U] |= std::uint64_t{1} << (character % 64U);
    }

    [[nodiscard]] bool matches(const simulation::SnesPpuState& ppu) const noexcept {
        for (unsigned group = 0U; group < used_.size(); ++group) {
            auto remaining = used_[group];
            while (remaining != 0U) {
                const auto character = group * 64U + std::countr_zero(remaining);
                remaining &= remaining - 1U;
                const auto address = (std::uint32_t(ppu.bg2_character_base) * 2U
                    + character * 32U) & 65535U;
                const auto first = std::min<std::uint32_t>(32U, 65536U - address);
                const auto* source = bytes_.data() + character * 32U;
                if (std::memcmp(source, ppu.vram.data() + address, first) != 0
                    || (first != 32U && std::memcmp(source + first,
                        ppu.vram.data(), 32U - first) != 0)) return false;
            }
        }
        return true;
    }

private:
    std::array<std::uint8_t, 1024U * 32U> bytes_{};
    std::array<std::uint64_t, 16U> used_{};
};

} // namespace starfox::platform_3ds
