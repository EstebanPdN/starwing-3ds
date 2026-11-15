#pragma once

#include "starfox/assets/rom.hpp"

#include <cstdint>
#include <vector>

// Native decoder for the cartridge's crunched graphics archives (.CCR/.PCR),
// replacing the Super FX routine in MDECRU.MC. Used by the CPU core's Super FX
// decrunch launch, the engine's screen loaders and host checks.
namespace starfox::assets {

struct DecrunchResult {
    std::vector<std::uint8_t> bytes;
    // First byte consumed from the LoROM image, expressed as an SNES address.
    std::uint32_t compressed_begin{};
};

// Decodes the backwards control stream used by MDECRU.MC. end_address is the
// assembler label immediately following a .CCR/.PCR archive. The stream must
// lie within the 64 KiB before end_address and decode to 1-65535 bytes.
// Throws std::runtime_error for a malformed stream and std::out_of_range when
// end_address is not a cartridge address inside the image.
[[nodiscard]] DecrunchResult decrunch_reverse(
    const RomImage& rom, std::uint32_t end_address);

} // namespace starfox::assets
