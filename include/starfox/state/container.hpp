#pragma once
#include <cstdint>
#include <span>
#include <vector>

// Save-state envelope used by the CPU, map VM, game state and SPC audio
// components and by the 3DS frontend's save storage.
namespace starfox::state {
// Envelope covers the header and payload with a checksum. Schema and ROM
// identity must match before any component is deserialized.
// Layout: magic "SFESTATE", then schema, ROM CRC-32 and payload length
// (little-endian u32), the payload, and a CRC-32 of all preceding bytes.
// pack throws std::runtime_error for payloads above 64 MiB.
[[nodiscard]] std::vector<std::uint8_t> pack(std::uint32_t schema,
    std::uint32_t rom_crc, std::span<const std::uint8_t> payload);
// Returns a view of the payload inside file. Throws std::runtime_error for
// another magic, schema or ROM CRC, a length mismatch or a bad checksum.
[[nodiscard]] std::span<const std::uint8_t> unpack(std::span<const std::uint8_t> file,
    std::uint32_t expected_schema, std::uint32_t expected_rom_crc);
} // namespace starfox::state
