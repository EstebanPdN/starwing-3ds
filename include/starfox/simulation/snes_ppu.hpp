#pragma once

#include <array>
#include <cstdint>

// The SNES PPU state that Wdc65816 builds from the cartridge's PPU register
// writes and DMA. The background/sprite renderers and the 3DS frontend read it
// through MapVm::ppu_state(); it is saved with the CPU state.

namespace starfox::simulation {

struct SnesPpuState {
    [[nodiscard]] bool operator==(const SnesPpuState&) const = default;
    // Byte-indexed: VRAM word address N is bytes 2N (low) and 2N + 1.
    std::array<std::uint8_t, 64U * 1024U> vram{};
    // BGR555 colours.
    std::array<std::uint16_t, 256> cgram{};
    // 512 bytes of sprite records, then the 32-byte high table.
    std::array<std::uint8_t, 544> oam{};
    // BGMODE ($2105): mode, BG3 priority and tile sizes. The character and
    // screen bases below are VRAM word addresses; *_screen_size holds the
    // BGnSC size bits.
    std::uint8_t background_mode{2U};
    bool bg3_high_priority{};
    bool bg1_tile_size_16{};
    bool bg2_tile_size_16{};
    bool bg3_tile_size_16{};
    bool bg4_tile_size_16{};
    std::uint8_t mosaic{};
    std::uint8_t object_select{3U};
    std::uint16_t bg1_character_base{};
    std::uint16_t bg1_screen_base{};
    std::uint8_t bg1_screen_size{};
    std::int16_t bg1_scroll_x{};
    std::int16_t bg1_scroll_y{};
    std::uint16_t bg2_character_base{0x5000U};
    std::uint16_t bg2_screen_base{0x7000U};
    std::uint8_t bg2_screen_size{3U};
    std::int16_t bg2_scroll_x{};
    std::int16_t bg2_scroll_y{};
    std::uint16_t bg3_screen_base{0x2c00U};
    std::uint8_t bg3_screen_size{3U};
    std::uint16_t bg3_character_base{0x7000U};
    std::int16_t bg3_scroll_x{};
    std::int16_t bg3_scroll_y{};
    // TM ($212c): the layers on the main screen.
    std::uint8_t main_screen{0x13U};
    // Mode 2 per-column BG2 offsets, kept in VRAM, are on (DOVOFS).
    bool bg2_vertical_offsets_enabled{};
    // Per-scanline BG2 horizontal scroll copied from the cartridge's HDMA
    // table (DOHOFS).
    std::array<std::int16_t, 224> bg2_horizontal_offsets{};
    bool bg2_horizontal_offsets_enabled{};
    // IRQ SETBG2VOFS: tunnel floor/ceiling rows select one of two tilemap
    // pages. These are discrete HDMA page selections, not roll offsets.
    std::array<std::int16_t, 224> bg2_scanline_scroll_y{};
    bool bg2_scanline_scroll_enabled{};
    // INATUNNEL is 1 in a mode 1 or 2 scene that is not a rotating
    // (ROTATE_HOF) landscape: a tunnel cross-section.
    bool tunnel_scene{};
};

} // namespace starfox::simulation
