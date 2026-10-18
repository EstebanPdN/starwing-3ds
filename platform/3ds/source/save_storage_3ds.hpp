#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace starfox::assets { class RomImage; }
namespace starfox::simulation { class GameSimulation; }

namespace starfox::platform_3ds {
class Audio3ds;

// Paths are supplied by the session; existing SD formats remain unchanged.
bool write_autosave(const char* path, const assets::RomImage& rom,
    const simulation::GameSimulation& game, const Audio3ds& audio);
bool restore_autosave(const char* path, const assets::RomImage& rom,
    simulation::GameSimulation& game, Audio3ds& audio);
std::vector<std::uint8_t> read_ex_save(const char* path);
void write_ex_save(std::span<const std::uint8_t> bytes, const char* path);

} // namespace starfox::platform_3ds
