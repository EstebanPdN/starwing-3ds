#include "asset_stage.h"
#include "asset_stage.hpp"
#include "rom_probe.h"

#include "starfox/assets/bps.hpp"
#include "starfox/assets/runtime_bundle.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ResourceName {
    int id;
    const char *path;
};

constexpr std::array resources{
    ResourceName{101, "romfs:/ultrastarfox-v12.bps"},
    ResourceName{102, "romfs:/ultrastarfox.txt"},
    ResourceName{108, "romfs:/starfox-ex-v12.bps"},
    ResourceName{109, "romfs:/starfox-ex.txt"},
    ResourceName{120, "romfs:/retail-japan-v10-to-usa-v12.bps"},
    ResourceName{121, "romfs:/retail-japan-v11-to-usa-v12.bps"},
    ResourceName{122, "romfs:/retail-usa-v10-to-v12.bps"},
    ResourceName{123, "romfs:/retail-usa-v11-to-v12.bps"},
    ResourceName{124, "romfs:/retail-europe-v10-to-usa-v12.bps"},
    ResourceName{125, "romfs:/retail-europe-v11-to-usa-v12.bps"},
    ResourceName{126, "romfs:/retail-germany-v10-to-usa-v12.bps"},
};

std::vector<std::uint8_t> read_file(const char *path) {
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) throw std::runtime_error(std::string("Cannot open ") + path);
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        throw std::runtime_error("Cannot seek input file");
    }
    const long size = std::ftell(file);
    if (size < 0 || size > 64L * 1024L * 1024L || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        throw std::runtime_error("Invalid input file size");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    const bool complete = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!complete) throw std::runtime_error("Cannot read input file");
    return bytes;
}

std::array<std::vector<std::uint8_t>, resources.size()> load_resources() {
    std::array<std::vector<std::uint8_t>, resources.size()> data;
    for (std::size_t i = 0; i < resources.size(); ++i)
        data[i] = read_file(resources[i].path);
    return data;
}

std::span<const std::uint8_t> resource(
    const std::array<std::vector<std::uint8_t>, resources.size()> &data, int id) {
    for (std::size_t i = 0; i < resources.size(); ++i)
        if (resources[i].id == id) return data[i];
    throw std::runtime_error("Unknown asset resource");
}

std::uint32_t manifest(
    const std::array<std::vector<std::uint8_t>, resources.size()> &data) {
    std::array<starfox::assets::RuntimeManifestResource, resources.size()> entries;
    for (std::size_t i = 0; i < resources.size(); ++i)
        entries[i] = {data[i], resources[i].id == 102 || resources[i].id == 109};
    return starfox::assets::runtime_asset_manifest(entries);
}

int regional_patch(std::uint32_t crc) {
    switch (crc) {
    case 0x8fc4e6d0u: return 0;
    case 0x41a60b3fu: return 120;
    case 0xad668a41u: return 121;
    case 0x0bae0941u: return 122;
    case 0xb18676b2u: return 123;
    case 0x865f1a71u: return 124;
    case 0xba64da2bu: return 125;
    case 0xb48ca238u: return 126;
    default: throw std::runtime_error("Unsupported retail ROM");
    }
}

std::string text_resource(
    const std::array<std::vector<std::uint8_t>, resources.size()> &data, int id) {
    const auto bytes = resource(data, id);
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

void write_atomically(const char *path, std::span<const std::uint8_t> bytes) {
    const std::string temporary = std::string(path) + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) throw std::runtime_error("Cannot create temporary asset bundle");
    const bool complete = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size()
        && std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!complete || !closed || std::rename(temporary.c_str(), path) != 0)
        throw std::runtime_error("Cannot finish asset bundle on SD");
}

void set_message(char *target, std::size_t capacity, const char *value) {
    if (target != nullptr && capacity != 0) std::snprintf(target, capacity, "%s", value);
}

} // namespace

starfox::assets::RuntimeBundlePayload starwing_load_runtime_payload(
    const char *path) {
    const auto data = load_resources();
    return starfox::assets::decode_runtime_bundle(
        read_file(path), manifest(data));
}

extern "C" int starwing_prepare_assets(const char *rom_path, const char *output_path,
    char *message, std::size_t message_capacity) {
    try {
        const auto data = load_resources();
        const auto expected_manifest = manifest(data);
        if (FILE *existing = std::fopen(output_path, "rb")) {
            std::fclose(existing);
            try {
                const auto bytes = read_file(output_path);
                static_cast<void>(starfox::assets::decode_runtime_bundle(bytes, expected_manifest));
                set_message(message, message_capacity, "Existing assets verified");
                return 1;
            } catch (const std::exception &) {
                if (rom_path == nullptr || *rom_path == '\0')
                    throw;
                /* A valid retail ROM can rebuild a derived, stale companion. */
            }
        }
        if (rom_path == nullptr || *rom_path == '\0')
            throw std::runtime_error("ROM required for first asset build");
        StarwingRomInfo info;
        if (!starwing_probe_rom(rom_path, &info))
            throw std::runtime_error("Unsupported or modified retail ROM");
        auto retail = read_file(rom_path);
        if (retail.size() == (1u << 20u) + 512u)
            retail.erase(retail.begin(), retail.begin() + 512);
        const int patch = regional_patch(info.crc32);
        if (patch != 0)
            retail = starfox::assets::apply_bps_patch(retail, resource(data, patch));
        if (retail.size() != (1u << 20u)
            || starfox::assets::crc32(retail) != 0x8fc4e6d0u)
            throw std::runtime_error("Regional canonicalization failed");
        starfox::assets::RuntimeBundlePayload payload;
        payload.original_rom = starfox::assets::apply_bps_patch(retail, resource(data, 101));
        payload.original_symbols = text_resource(data, 102);
        payload.starfox_ex_rom = starfox::assets::apply_bps_patch(retail, resource(data, 108));
        payload.starfox_ex_symbols = text_resource(data, 109);
        const auto bundle = starfox::assets::encode_runtime_bundle(payload, expected_manifest);
        static_cast<void>(starfox::assets::decode_runtime_bundle(bundle, expected_manifest));
        write_atomically(output_path, bundle);
        set_message(message, message_capacity, "Original and EX assets built");
        return 2;
    } catch (const std::exception &error) {
        set_message(message, message_capacity, error.what());
        return 0;
    }
}
