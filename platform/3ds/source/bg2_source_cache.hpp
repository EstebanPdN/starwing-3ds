#pragma once
#include "bg2_motion.hpp"
#include "starfox/simulation/snes_ppu.hpp"
#include <algorithm>
#include <array>
#include <cstring>

// Bg2SourceCache: byte copies of what the Mode 1 BG2 atlas depends on, so the atlas is rebuilt
// only when that VRAM changes. Used by mode1_bg2_atlas.hpp.

namespace starfox::platform_3ds {
// Dependencies of the unscrolled Mode 1 BG2 atlas: map words and the 4-bpp
// characters they address. Other layers' VRAM writes do not change this art.
// Palette and display metadata are checked by the owning frame cache.
class Bg2SourceCache {
public:
    // True when BG2's main-screen enable, bases, map size and tile size match
    // the capture and, while BG2 is enabled, the map words and referenced
    // characters are byte-identical.
    bool matches(const simulation::SnesPpuState& ppu) const noexcept {
        if (!valid_ || enabled_ != bool(ppu.main_screen&2U) || base_ != ppu.bg2_character_base || map_ != ppu.bg2_screen_base
            || size_ != ppu.bg2_screen_size || large_ != ppu.bg2_tile_size_16) return false;
        if (!(ppu.main_screen&2U)) return true;
        const auto address = (unsigned(map_) * 2U) & 65535U;
        if (!same(ppu, address, words_.data(), word_bytes_)) return false;
        for (std::size_t i=0; i<count_; ++i) {
            const auto character = characters_[i];
            if (!same(ppu, (unsigned(base_)*2U + character*32U)&65535U,
                    bytes_.data()+character*32U, 32U)) return false;
        }
        return true;
    }
    // matches(ppu) for a caller that names the VRAM contents with the
    // simulation's revision: under the revision these dependencies last
    // compared equal, they still are, unless one of them lies on the roll
    // table a presentation PPU may interpolate. A zero revision compares.
    bool matches(const simulation::SnesPpuState& ppu,
        std::uint64_t vram_revision) const noexcept {
        if (vram_revision != 0U && verified_revision_ == vram_revision
            && valid_ && roll_independent_ && enabled_ == bool(ppu.main_screen&2U)
            && base_ == ppu.bg2_character_base && map_ == ppu.bg2_screen_base
            && size_ == ppu.bg2_screen_size && large_ == ppu.bg2_tile_size_16) return true;
        const bool same = matches(ppu);
        verified_revision_ = same && roll_independent_ ? vram_revision : 0U;
        return same;
    }
    // capture(ppu) that also records `vram_revision` for the shortcut above.
    void capture(const simulation::SnesPpuState& ppu,
        std::uint64_t vram_revision) noexcept {
        capture(ppu);
        if (valid_ && roll_independent_) verified_revision_ = vram_revision;
    }
    // Copies the map words and every character they address (all four for a
    // 16x16 tile). With BG2 off the main screen only that state is recorded.
    void capture(const simulation::SnesPpuState& ppu) noexcept {
        verified_revision_ = 0U;
        roll_independent_ = true;
        enabled_=bool(ppu.main_screen&2U);
        base_=ppu.bg2_character_base; map_=ppu.bg2_screen_base;
        size_=ppu.bg2_screen_size; large_=ppu.bg2_tile_size_16;
        if (!(ppu.main_screen&2U)) { valid_=true;return; }
        word_bytes_=2048U * ((size_&1U)?2U:1U) * ((size_&2U)?2U:1U);
        copy(ppu,(unsigned(map_)*2U)&65535U,words_.data(),word_bytes_);
        if (overlaps_roll_table((unsigned(map_)*2U)&65535U,word_bytes_))
            roll_independent_ = false;
        std::array<bool,1024> used{}; count_=0;
        for (std::size_t i=0; i<word_bytes_; i+=2U) {
            const unsigned tile=(words_[i] | (unsigned(words_[i+1])<<8U))&1023U;
            for (unsigned y=0; y<(large_?2U:1U); ++y)
                for (unsigned x=0; x<(large_?2U:1U); ++x) {
                    const auto character=(tile+x+16U*y)&1023U;
                    if (used[character]) continue;
                    used[character]=true; characters_[count_++]=character;
                    copy(ppu,(unsigned(base_)*2U+character*32U)&65535U,
                        bytes_.data()+character*32U,32U);
                    if (overlaps_roll_table((unsigned(base_)*2U+character*32U)&65535U,32U))
                        roll_independent_ = false;
                }
        }
        valid_=true;
    }
private:
    static bool same(const simulation::SnesPpuState& ppu,unsigned address,
        const std::uint8_t* bytes,std::size_t count) noexcept {
        const auto first=std::min<std::size_t>(count,65536U-address);
        return std::memcmp(bytes,ppu.vram.data()+address,first)==0
            && (first==count || std::memcmp(bytes+first,ppu.vram.data(),count-first)==0);
    }
    static void copy(const simulation::SnesPpuState& ppu,unsigned address,
        std::uint8_t* bytes,std::size_t count) noexcept {
        const auto first=std::min<std::size_t>(count,65536U-address);
        std::memcpy(bytes,ppu.vram.data()+address,first);
        if(first!=count) std::memcpy(bytes+first,ppu.vram.data(),count-first);
    }
    // Whether `count` VRAM bytes from `address` (wrapping at 64 KiB) touch
    // the roll table that Bg2Motion interpolates.
    static bool overlaps_roll_table(std::size_t address,std::size_t count) noexcept {
        constexpr std::size_t first=Bg2Motion::roll_table_byte;
        constexpr std::size_t last=first+Bg2Motion::roll_table_bytes;
        const auto overlap=[](std::size_t begin,std::size_t end) {
            return begin<last && end>first;
        };
        const auto end=address+count;
        return end<=65536U ? overlap(address,end)
            : overlap(address,65536U) || overlap(0U,end-65536U);
    }
    std::array<std::uint8_t,8192> words_{};
    std::array<std::uint8_t,32768> bytes_{};
    std::array<std::uint16_t,1024> characters_{};
    std::size_t word_bytes_{},count_{};
    // The VRAM revision under which the dependencies last compared equal;
    // zero when unknown. Bookkeeping only, hence mutable.
    mutable std::uint64_t verified_revision_{};
    std::uint16_t base_{},map_{};
    std::uint8_t size_{};
    bool large_{},valid_{},enabled_{},roll_independent_{};
};
}
