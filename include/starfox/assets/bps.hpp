#pragma once

#include <cstdint>
#include <span>
#include <vector>

// BPS1 patching and CRC-32. The 3DS asset stage patches the user's retail ROM
// into the Original and Star Fox EX images; ROM identity, save-state envelopes
// and the asset bundle use crc32().
namespace starfox::assets {

// Apply a BPS1 patch and validate its source, target, and patch CRC32 values.
// The returned image is the exact target byte stream described by the patch.
// Throws std::runtime_error for a malformed patch, a source of the wrong size
// or checksum, or a target checksum mismatch.
[[nodiscard]] std::vector<std::uint8_t> apply_bps_patch(
    std::span<const std::uint8_t> source,
    std::span<const std::uint8_t> patch);

// Standard CRC-32 (reflected polynomial 0xEDB88320, initial value and final
// XOR 0xFFFFFFFF), the checksum BPS files store.
[[nodiscard]] std::uint32_t crc32(
    std::span<const std::uint8_t> bytes) noexcept;

} // namespace starfox::assets
