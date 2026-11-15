#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace starfox::assets { class RomImage; }
namespace starfox::simulation { class GameSimulation; }

// Autosave checkpoints and Star Fox EX save RAM on the SD card, for
// game_3ds.cpp.

namespace starfox::platform_3ds {
class Audio3ds;

// Paths are supplied by the session; existing SD formats remain unchanged.
// A checkpoint holds the game and SPC state, tagged with the ROM's CRC-32. It
// is written to `path`.tmp and renamed over `path`; false on any failure, which
// leaves the previous file in place.
bool write_autosave(const char* path, const assets::RomImage& rom,
    const simulation::GameSimulation& game, const Audio3ds& audio);
// Restores a checkpoint written for this ROM. Returns false, keeping the current
// state, for a missing, empty, larger than 16 MiB or invalid file.
bool restore_autosave(const char* path, const assets::RomImage& rom,
    simulation::GameSimulation& game, Audio3ds& audio);
// The 64 KiB EX save RAM, or zeros when the file is missing or shorter.
std::vector<std::uint8_t> read_ex_save(const char* path);
// Writes exactly 64 KiB through `path`.tmp and a rename; ignores other sizes
// and failures.
void write_ex_save(std::span<const std::uint8_t> bytes, const char* path);

} // namespace starfox::platform_3ds
