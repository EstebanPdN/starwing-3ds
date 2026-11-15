#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Starfox-Assets.BIN, the SD companion holding the patched Original and Star
// Fox EX ROMs with their symbol listings. The 3DS asset stage builds and
// verifies it; host tests round-trip it.
namespace starfox::assets {

// Each patched ROM image and its assembler symbol listing (text).
struct RuntimeBundlePayload {
    std::vector<std::uint8_t> original_rom;
    std::string original_symbols;
    std::vector<std::uint8_t> starfox_ex_rom;
    std::string starfox_ex_symbols;
};

// One embedded resource hashed into the manifest. Text resources set
// normalize_line_endings, which hashes CRLF and a lone CR as LF.
struct RuntimeManifestResource {
    std::span<const std::uint8_t> bytes;
    bool normalize_line_endings = false;
};

// Produces a platform-independent fingerprint for the embedded patches and
// symbol tables used to build Starfox-Assets.BIN. Text resources are
// normalized so Windows CRLF and Unix LF checkouts remain compatible.
// The CRC-32 covers each resource's length and bytes in sequence, so the
// resource order is part of the result.
[[nodiscard]] std::uint32_t runtime_asset_manifest(
    std::span<const RuntimeManifestResource> resources);

// Shared resource order for the bundle builder and the runtime.
// Missing resources reject rather than producing a partial compatibility ID.
// resource(id) returns embedded resource id: 101/108 the Original/EX patches,
// 102/109 their symbol listings (hashed as text) and 120-126 the regional
// patches to USA v1.2. Throws std::invalid_argument for a null provider and
// std::runtime_error for an empty resource.
[[nodiscard]] std::uint32_t runtime_companion_manifest(
    std::span<const std::uint8_t> (*resource)(int));

// Starfox-Assets.BIN is generated locally from the user's retail ROM, first
// converted to USA v1.2. The manifest binds it to the precise embedded
// patches/symbol tables in the executable, so stale companions cannot
// silently mix incompatible code/data.
// Layout: magic "SFOXAS01", the manifest, four payload sizes and four CRC-32s
// (little-endian u32), the payloads in RuntimeBundlePayload order, then a
// CRC-32 of all preceding bytes. Throws std::runtime_error above 64 MiB.
[[nodiscard]] std::vector<std::uint8_t> encode_runtime_bundle(
    const RuntimeBundlePayload& payload,
    std::uint32_t manifest);

// Checks magic, manifest, sizes and every CRC-32, then returns copies of the
// payloads. Throws std::runtime_error with a user-facing message on any
// mismatch, including a companion built for a different manifest.
[[nodiscard]] RuntimeBundlePayload decode_runtime_bundle(
    std::span<const std::uint8_t> bytes,
    std::uint32_t expected_manifest);

} // namespace starfox::assets
