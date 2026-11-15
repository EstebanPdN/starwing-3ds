#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Read-only cartridge access for the engine, the 3DS frontend and host
// tests/tools: a headerless LoROM image and its assembler symbol listing.
// SNES addresses are 24-bit bank:offset values.
namespace starfox::assets {

// Owns an immutable LoROM image whose size is a multiple of 32 KiB.
class RomImage {
public:
    // Throws std::invalid_argument for an empty image or one that still has
    // a copier header (size not a multiple of 32 KiB).
    explicit RomImage(std::vector<std::uint8_t> bytes);

    // Reads the whole file; throws std::runtime_error if it cannot be opened.
    [[nodiscard]] static RomImage load(const std::filesystem::path& path);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    // Cartridge bytes are immutable. Nested save-state components share
    // this identity instead of rescanning the entire ROM at every save.
    [[nodiscard]] std::uint32_t crc32() const noexcept { return crc32_; }
    // File offset of a LoROM address: offsets $8000-$ffff of each bank, with
    // bank bit 7 ignored ($80-$ff mirror $00-$7f). Throws std::out_of_range
    // below $8000 or past the end of the image.
    [[nodiscard]] std::size_t lorom_offset(std::uint32_t snes_address) const;
    // Little-endian reads. Unlike lorom_offset(), an offset below $8000 reads
    // as 0 (open bus); an address past the image still throws.
    [[nodiscard]] std::uint8_t read8(std::uint32_t snes_address) const;
    [[nodiscard]] std::uint16_t read16(std::uint32_t snes_address) const;
    [[nodiscard]] std::int16_t read_i16(std::uint32_t snes_address) const;

private:
    std::vector<std::uint8_t> bytes_;
    std::uint32_t crc32_{};
};

// Assembler symbol listing. Each "NAME $HEX" row adds an address to the
// upper-cased name (a label can be listed in several banks); other rows are
// skipped. Lookups are case-insensitive.
class SymbolMap {
public:
    using Entries = std::unordered_map<std::string, std::vector<std::uint32_t>>;

    // Throws std::runtime_error if the file cannot be opened.
    [[nodiscard]] static SymbolMap load(const std::filesystem::path& path);
    [[nodiscard]] static SymbolMap parse(std::string_view text);
    // Addresses in listing order; empty for an unknown name.
    [[nodiscard]] const std::vector<std::uint32_t>& find(const std::string& name) const;
    [[nodiscard]] const Entries& entries() const noexcept;

private:
    Entries symbols_;
};

} // namespace starfox::assets
