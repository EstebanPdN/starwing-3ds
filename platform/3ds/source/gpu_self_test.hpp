#pragma once
#include <string>
#include <span>
#include <initializer_list>
#include "starfox/simulation/snes_ppu.hpp"
#include "starfox/render/raster_commands.hpp"
#include "starfox/render/palette.hpp"
// Pixel diagnostics written by the START+SELECT test tour (game_3ds.cpp) and by
// the device program tests/display_gpu_replay.cpp. Each function writes files
// into an existing SD folder; a file that cannot be written is skipped.
namespace starfox::platform_3ds {
class Display3ds;
// Appends "<sha256>  <name>" to folder/SHA256SUMS.txt for each named file that
// exists in the folder and reads back without error.
void append_diagnostic_hashes(const std::string&,std::initializer_list<const char*>);
// Saves the PPU state, presentation-state.json and, when native, the raster
// commands and texels that produced the captured picture.
void write_presentation_snapshot(const std::string&,const simulation::SnesPpuState&,
    int scroll_x,int scroll_y,double alpha,const render::RasterCommands&,bool native);
// Writes pixel-comparison.json with status "skipped" and the given reason.
void write_pixel_comparison_skipped(const std::string&,const char* reason);
// ROM-free patterns through the production presenter. Only on explicit test
// entry: SD writes and readback never enter the measured gameplay windows.
// Writes folder/gpu-pixels.jsonl; after the cases the display's wide mode is set
// back to restore_wide.
void run_gpu_self_test(Display3ds&,const std::string& folder,bool restore_wide);
// Compares `actual`, the top framebuffer as scanned out (400x240 RGB565, columns
// contiguous, Y reversed), with the CPU-rendered `reference`. Writes
// pixel-comparison.json, reference.rgb565 and pixel-diff.pgm; other sizes are
// ignored. The reference comes from this port's renderer, not an SNES.
void write_game_pixel_comparison(const std::string& folder,
    const render::Framebuffer& reference,const render::Palette256&,
    std::span<const std::uint8_t> actual,bool wide,const char* backend,double alpha);
}
