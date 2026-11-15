#pragma once

#include "bg2_atlas_cache.hpp"
#include "bg2_motion.hpp"

namespace starfox::platform_3ds {

// The complete BG2 image depends on map words and referenced character
// bytes. Roll/HDMA tables can change VRAM without changing this image.
class Bg2MapSourceCache {
public:
    void reset() noexcept { valid_ = false; verified_revision_ = 0U; }
    [[nodiscard]] bool all_opaque() const noexcept { return valid_ && all_opaque_; }
    [[nodiscard]] std::uint8_t sample(unsigned x,unsigned y,bool* uniform=nullptr,bool* opaque=nullptr) const noexcept {
        const unsigned width=(screen_size_&1U)?512U:256U;
        const auto flags=opaque_tiles_[(y/8U)*(width/8U)+x/8U];
        if (opaque) *opaque=(flags&1U)!=0U;
        if (uniform) *uniform=(flags&2U)!=0U;
        return indices_[std::size_t(y)*width+x];
    }
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
    // matches(ppu) for a caller that also names the VRAM contents. The
    // simulation's VRAM revision is unique to one content history, so the
    // bytes this cache depends on are the ones it compared last time under
    // the same revision. The one difference a presentation PPU may carry
    // under an unchanged revision is BG2's interpolated roll table; the
    // shortcut is taken only when no map word and no referenced character
    // overlaps that table. A zero revision always compares.
    [[nodiscard]] bool matches(const simulation::SnesPpuState& ppu,
        std::uint64_t vram_revision) const noexcept {
        if (vram_revision != 0U && verified_revision_ == vram_revision
            && valid_ && roll_independent_ && !ppu.bg2_tile_size_16
            && ppu.bg2_character_base == character_base_
            && ppu.bg2_screen_base == screen_base_
            && ppu.bg2_screen_size == screen_size_) return true;
        const bool same = matches(ppu);
        verified_revision_ = same && roll_independent_ ? vram_revision : 0U;
        return same;
    }
    // Bring the cache to this PPU's BG2 source. True when it was rebuilt:
    // whatever was derived from the previous source is then stale.
    bool ensure(const simulation::SnesPpuState& ppu,
        std::uint64_t vram_revision) noexcept {
        if (matches(ppu, vram_revision)) return false;
        remember(ppu);
        if (valid_ && roll_independent_) verified_revision_ = vram_revision;
        return true;
    }
    __attribute__((noinline)) void remember(const simulation::SnesPpuState& ppu) noexcept {
        valid_ = false;
        verified_revision_ = 0U;
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
        roll_independent_ = !overlaps_roll_table(address, bytes_);
        for (std::size_t i = 0U; i < bytes_; i += 2U) {
            const auto character = static_cast<std::uint16_t>(words_[i]
                | (std::uint16_t(words_[i + 1U]) << 8U)) & 1023U;
            const auto bit = std::uint64_t{1} << (character & 63U);
            auto& used = examined[character >> 6U];
            if ((used & bit) != 0U) continue;
            used |= bit;
            characters_.remember(ppu, character);
            if (overlaps_roll_table((std::uint32_t(character_base_) * 2U
                    + character * 32U) & 65535U, 32U)) roll_independent_ = false;
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
        // Keep the exact indexed source beside its byte certificate. The
        // rolled-ground planner previously decoded/located map pixels for
        // every output column on every frame, even when VRAM was unchanged.
        std::array<std::uint8_t,1024U*64U> decoded;
        std::array<std::uint8_t,1024U> opaque{};
        std::array<std::uint64_t,16U> decoded_valid{};
        const unsigned width_tiles=(screen_size_&1U)?64U:32U;
        const unsigned height_tiles=(screen_size_&2U)?64U:32U;
        for (unsigned ty=0;ty<height_tiles;++ty) for(unsigned tx=0;tx<width_tiles;++tx) {
            const unsigned entry=((tx/32U)+(ty/32U)*(width_tiles/32U))*1024U
                +(ty&31U)*32U+(tx&31U);
            const unsigned tile=words_[entry*2U]|(unsigned(words_[entry*2U+1U])<<8U);
            const unsigned character=tile&1023U;
            auto* pixels=decoded.data()+character*64U;
            const auto bit=std::uint64_t{1}<<(character&63U);
            if (!(decoded_valid[character/64U]&bit)) {
                decoded_valid[character/64U]|=bit;
                opaque[character]=1;
                const unsigned base=(unsigned(character_base_)*2U+character*32U)&65535U;
                for(unsigned y=0;y<8U;++y)for(unsigned x=0;x<8U;++x) {
                    unsigned colour=0;
                    for(unsigned plane=0;plane<4U;++plane) {
                        const unsigned at=(base+y*2U+(plane&1U)+(plane/2U)*16U)&65535U;
                        colour|=((ppu.vram[at]>>(7U-x))&1U)<<plane;
                    }
                    pixels[y*8U+x]=std::uint8_t(colour);
                    opaque[character]&=colour!=0U;
                }
                if(std::all_of(pixels+1,pixels+64,[&](auto c){return c==pixels[0];}))opaque[character]|=2U;
            }
            opaque_tiles_[ty*width_tiles+tx]=opaque[character];
            for(unsigned y=0;y<8U;++y)for(unsigned x=0;x<8U;++x) {
                const auto colour=pixels[((tile&0x8000U)?7U-y:y)*8U+((tile&0x4000U)?7U-x:x)];
                indices_[std::size_t(ty*8U+y)*(width_tiles*8U)+tx*8U+x]
                    =colour?std::uint8_t(((tile>>10U)&7U)*16U+colour):0U;
            }
        }
        valid_ = true;
    }
private:
    // Whether `count` VRAM bytes from `address` (wrapping at 64 KiB) touch
    // the roll table that Bg2Motion interpolates.
    [[nodiscard]] static bool overlaps_roll_table(std::size_t address,
        std::size_t count) noexcept {
        constexpr std::size_t first = Bg2Motion::roll_table_byte;
        constexpr std::size_t last = first + Bg2Motion::roll_table_bytes;
        const auto overlap = [](std::size_t begin, std::size_t end) {
            return begin < last && end > first;
        };
        const auto end = address + count;
        return end <= 65536U ? overlap(address, end)
            : overlap(address, 65536U) || overlap(0U, end - 65536U);
    }
    Bg2AtlasSourceCache characters_{};
    // The VRAM revision under which the dependencies last compared equal;
    // zero when unknown. Bookkeeping only, hence mutable.
    mutable std::uint64_t verified_revision_{};
    bool roll_independent_{};
    std::array<std::uint8_t, 8192U> words_{};
    std::array<std::uint8_t,512U*512U> indices_{};
    std::array<std::uint8_t,4096U> opaque_tiles_{};
    std::size_t bytes_{};
    std::uint16_t character_base_{}, screen_base_{};
    std::uint8_t screen_size_{};
    bool valid_{};
    bool all_opaque_{};
};
} // namespace starfox::platform_3ds
