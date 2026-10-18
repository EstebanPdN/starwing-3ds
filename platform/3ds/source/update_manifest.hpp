#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace starfox::platform_3ds {
inline constexpr std::uint64_t update_title_id = 0x0004000005f58b00ULL;
inline constexpr std::uint32_t update_max_bytes = 64U * 1024U * 1024U;
inline constexpr const char* update_api =
    "https://api.github.com/repos/EstebanPdN/starwing-3ds/releases/latest";
inline constexpr const char* update_experimental_api =
    "https://api.github.com/repos/EstebanPdN/starwing-3ds/releases?per_page=100";

struct UpdateRelease {
    std::string version, url, sha256;
    std::uint32_t bytes{};
    std::string notes;
};

std::optional<std::array<unsigned, 4>> update_version(std::string_view text);
bool update_is_newer(std::string_view candidate, std::string_view installed);
std::optional<UpdateRelease> parse_update_release(std::string_view json, bool homebrew,
    bool experimental = false);
} // namespace starfox::platform_3ds
