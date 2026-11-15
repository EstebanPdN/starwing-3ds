#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// GitHub release metadata for the in-game updater: version tags, release
// selection and the asset to download. Used by updater_3ds.cpp; host-tested by
// tests/update_manifest_test.cpp and tests/updater_install_test.cpp.

namespace starfox::platform_3ds {
// The title ID a CIA update must carry, and the largest accepted asset.
inline constexpr std::uint64_t update_title_id = 0x0004000005f58b00ULL;
inline constexpr std::uint32_t update_max_bytes = 64U * 1024U * 1024U;
// The stable channel reads the latest release; the experimental channel lists
// releases and takes the newest prerelease.
inline constexpr const char* update_api =
    "https://api.github.com/repos/EstebanPdN/starwing-3ds/releases/latest";
inline constexpr const char* update_experimental_api =
    "https://api.github.com/repos/EstebanPdN/starwing-3ds/releases?per_page=100";

// version is the release tag, sha256 the asset digest in lowercase hex, bytes
// the asset size and notes the release body (at most 12288 bytes).
struct UpdateRelease {
    std::string version, url, sha256;
    std::uint32_t bytes{};
    std::string notes;
};

// Parses "[v]MAJOR[.MINOR[.PATCH]]" with an optional "-E<n>" experimental
// suffix; every number is at most 65535 and n is at least 1. The fourth part
// is n, or UINT_MAX for a stable tag, so a stable version sorts after its
// experimental builds. Returns nothing for any other text.
std::optional<std::array<unsigned, 4>> update_version(std::string_view text);
// True when both versions parse and candidate sorts after installed.
bool update_is_newer(std::string_view candidate, std::string_view installed);
// Reads one release object or an array of up to 100 (the newest qualifying
// release wins). A release qualifies when it is not a draft, its prerelease
// flag and "-E" tag match `experimental`, and it has exactly one asset named
// Starwing-3DS.cia (Starwing-3DS.3dsx for homebrew) at the expected download
// URL, with a size up to update_max_bytes and a sha256 digest. Nothing is
// returned for malformed JSON, duplicate keys or input over 2 MiB.
std::optional<UpdateRelease> parse_update_release(std::string_view json, bool homebrew,
    bool experimental = false);
} // namespace starfox::platform_3ds
