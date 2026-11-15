#pragma once
#include "starfox/simulation/snes_ppu.hpp"
#include <algorithm>
#include <array>
#include <cstring>

// Bg3AtlasSourceCache: byte copies of what the Mode 1 BG3 atlas depends on, so the atlas is
// rebuilt only when that VRAM changes. Used by frame_3ds; host-tested by
// tests/bg3_atlas_cache_test.cpp.

namespace starfox::platform_3ds {
// Dependencies of the unscrolled Mode 1 BG3 atlas: map words and the 2-bpp
// characters they address. Other layers' VRAM writes do not change this art.
// Palette and display metadata are checked by the owning frame cache.
class Bg3AtlasSourceCache {
public:
    // True when BG3's bases, map size and tile size match the capture and the
    // map words and referenced characters are byte-identical.
    bool matches(const simulation::SnesPpuState& ppu) const noexcept {
        if (!valid_ || base_ != ppu.bg3_character_base || map_ != ppu.bg3_screen_base
            || size_ != ppu.bg3_screen_size || large_ != ppu.bg3_tile_size_16) return false;
        const auto address = (unsigned(map_) * 2U) & 65535U;
        if (!same(ppu, address, words_.data(), word_bytes_)) return false;
        for (std::size_t i=0; i<count_; ++i) {
            const auto character = characters_[i];
            if (!same(ppu, (unsigned(base_)*2U + character*16U)&65535U,
                    bytes_.data()+character*16U, 16U)) return false;
        }
        return true;
    }
    // Copies the map words and every character they address (all four for a
    // 16x16 tile).
    void capture(const simulation::SnesPpuState& ppu) noexcept {
        base_=ppu.bg3_character_base; map_=ppu.bg3_screen_base;
        size_=ppu.bg3_screen_size; large_=ppu.bg3_tile_size_16;
        word_bytes_=2048U * ((size_&1U)?2U:1U) * ((size_&2U)?2U:1U);
        copy(ppu,(unsigned(map_)*2U)&65535U,words_.data(),word_bytes_);
        std::array<bool,1024> used{}; count_=0;
        for (std::size_t i=0; i<word_bytes_; i+=2U) {
            const unsigned tile=(words_[i] | (unsigned(words_[i+1])<<8U))&1023U;
            for (unsigned y=0; y<(large_?2U:1U); ++y)
                for (unsigned x=0; x<(large_?2U:1U); ++x) {
                    const auto character=(tile+x+16U*y)&1023U;
                    if (used[character]) continue;
                    used[character]=true; characters_[count_++]=character;
                    copy(ppu,(unsigned(base_)*2U+character*16U)&65535U,
                        bytes_.data()+character*16U,16U);
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
    std::array<std::uint8_t,8192> words_{};
    std::array<std::uint8_t,16384> bytes_{};
    std::array<std::uint16_t,1024> characters_{};
    std::size_t word_bytes_{},count_{};
    std::uint16_t base_{},map_{};
    std::uint8_t size_{};
    bool large_{},valid_{};
};
}
