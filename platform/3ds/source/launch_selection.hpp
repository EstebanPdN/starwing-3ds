#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace starfox::platform_3ds {
// Store only a basename; startup must still validate it against scanned ROMs.
struct LaunchSelection { std::string filename; bool ex{}; };
inline bool valid_rom_basename(const std::string& name) {
    return !name.empty() && name.size() <= 255 && name != "." && name != ".."
        && name.find_first_of("/\\\r\n:") == std::string::npos;
}
inline LaunchSelection read_launch_selection(const char* path) {
    LaunchSelection result;
    if (auto* file = std::fopen(path, "rb")) {
        char name[258]{}, mode[16]{};
        if (std::fgets(name, sizeof(name), file)
            && std::fgets(mode, sizeof(mode), file)) {
            std::string value{name};
            if (!value.empty() && value.back() == '\n') value.pop_back();
            if (valid_rom_basename(value)
                && (std::string{mode} == "0\n" || std::string{mode} == "1\n"))
                result = {value, mode[0] == '1'};
        }
        std::fclose(file);
    }
    return result;
}
inline bool write_launch_selection(const char* path, const LaunchSelection& value) {
    if (!valid_rom_basename(value.filename)) return false;
    const auto temporary = std::string{path} + ".tmp";
    auto* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool wrote = std::fprintf(file, "%s\n%d\n", value.filename.c_str(), value.ex ? 1 : 0) > 0;
    const bool closed = std::fclose(file) == 0;
    if (wrote && closed && std::rename(temporary.c_str(), path) == 0) return true;
    std::remove(temporary.c_str());
    // FAT-backed devoptab implementations may refuse rename-over-existing.
    // The selection is a tiny preference file, so fall back to writing it
    // directly when atomic replacement is unavailable.
    file = std::fopen(path, "wb");
    if (!file) return false;
    const bool fallback_wrote = std::fprintf(file, "%s\n%d\n",
        value.filename.c_str(), value.ex ? 1 : 0) > 0;
    return std::fclose(file) == 0 && fallback_wrote;
}
inline bool selection_available(const LaunchSelection& selection,
    const std::vector<std::string>& filenames) {
    return valid_rom_basename(selection.filename)
        && std::find(filenames.begin(), filenames.end(), selection.filename) != filenames.end();
}
}
