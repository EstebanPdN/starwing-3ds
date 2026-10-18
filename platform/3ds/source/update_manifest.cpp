#include "update_manifest.hpp"

#include <jansson.h>
#include <algorithm>
#include <memory>
#include <cstring>
#include <limits>

namespace starfox::platform_3ds {
namespace {
const char* json_text(const json_t* object, const char* key) {
    const auto* value = json_object_get(object, key);
    const auto* text = json_string_value(value);
    return text && json_string_length(value) == std::strlen(text) ? text : nullptr;
}
}
std::optional<std::array<unsigned, 4>> update_version(std::string_view text) {
    if (text.starts_with('v')) text.remove_prefix(1);
    std::array<unsigned, 4> parts{0, 0, 0, std::numeric_limits<unsigned>::max()};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned digits = 0;
        while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
            if (++digits > 5) return {};
            parts[i] = parts[i] * 10U + unsigned(text.front() - '0');
            if (parts[i] > 65535U) return {};
            text.remove_prefix(1);
        }
        if (!digits) return {};
        if (text.empty()) return parts;
        if (text.starts_with("-E")) {
            text.remove_prefix(2); parts[3] = 0;
            if (text.empty() || text.size() > 5) return {};
            for (char c : text) {
                if (c < '0' || c > '9') return {};
                parts[3] = parts[3] * 10U + unsigned(c - '0');
                if (parts[3] > 65535U) return {};
            }
            return parts[3] ? std::optional(parts) : std::nullopt;
        }
        if (text.front() != '.' || i == 2) return {};
        text.remove_prefix(1);
    }
    return {};
}

bool update_is_newer(std::string_view candidate, std::string_view installed) {
    const auto a = update_version(candidate), b = update_version(installed);
    return a && b && *a > *b;
}

namespace {
std::optional<UpdateRelease> parse_asset(const json_t* root, bool homebrew, bool experimental) {
    if (!json_is_object(root) || !json_is_false(json_object_get(root, "draft"))
        || !json_is_boolean(json_object_get(root, "prerelease"))
        || json_is_true(json_object_get(root, "prerelease")) != experimental) return {};
    const auto* tag = json_text(root, "tag_name");
    if (!tag || !update_version(tag)
        || (std::string_view(tag).find("-E") != std::string_view::npos) != experimental) return {};
    const auto* assets = json_object_get(root, "assets");
    if (!json_is_array(assets) || json_array_size(assets) > 64U) return {};
    const std::string filename = homebrew ? "Starwing-3DS.3dsx" : "Starwing-3DS.cia";
    const std::string expected = "https://github.com/EstebanPdN/starwing-3ds/releases/download/"
        + std::string(tag) + "/" + filename;
    std::optional<UpdateRelease> result;
    for (std::size_t i = 0; i < json_array_size(assets); ++i) {
        const auto* asset = json_array_get(assets, i);
        const auto* name = json_text(asset, "name");
        if (!name || filename != name) continue;
        if (result) return {};
        const auto* url = json_text(asset, "browser_download_url");
        const auto* digest = json_text(asset, "digest");
        const auto* size = json_object_get(asset, "size");
        if (!url || expected != url || !digest || !json_is_integer(size)) return {};
        const auto bytes = json_integer_value(size);
        const std::string_view hash(digest);
        if (bytes <= 0 || bytes > update_max_bytes || hash.size() != 71U
            || !hash.starts_with("sha256:")
            || !std::all_of(hash.begin() + 7, hash.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) return {};
        result = UpdateRelease{tag, url, std::string(hash.substr(7)),
            static_cast<std::uint32_t>(bytes), {}};
        if (const auto* notes = json_text(root, "body"))
            result->notes.assign(notes, std::min<std::size_t>(std::strlen(notes), 12288U));
    }
    return result;
}
}
std::optional<UpdateRelease> parse_update_release(std::string_view data, bool homebrew,
    bool experimental) {
    if (data.empty() || data.size() > 2U * 1024U * 1024U) return {};
    json_error_t error{};
    std::unique_ptr<json_t, decltype(&json_decref)> root(
        json_loadb(data.data(), data.size(), JSON_REJECT_DUPLICATES, &error), json_decref);
    if (!root) return {};
    if (!json_is_array(root.get())) return parse_asset(root.get(), homebrew, experimental);
    if (json_array_size(root.get()) > 100U) return {};
    std::optional<UpdateRelease> selected;
    for (std::size_t i = 0; i < json_array_size(root.get()); ++i) {
        auto candidate = parse_asset(json_array_get(root.get(), i), homebrew, experimental);
        if (candidate && (!selected || update_is_newer(candidate->version, selected->version)))
            selected = std::move(candidate);
    }
    return selected;
}
} // namespace starfox::platform_3ds
