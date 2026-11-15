#pragma once
#include <string>
#include <span>
#include <initializer_list>
#include "starfox/simulation/snes_ppu.hpp"
#include "starfox/render/raster_commands.hpp"
#include "starfox/render/palette.hpp"
namespace starfox::platform_3ds {
class Display3ds;
void append_diagnostic_hashes(const std::string&,std::initializer_list<const char*>);
void write_presentation_snapshot(const std::string&,const simulation::SnesPpuState&,
    int scroll_x,int scroll_y,double alpha,const render::RasterCommands&,bool native);
void write_pixel_comparison_skipped(const std::string&,const char* reason);
// ROM-free patterns through the production presenter. Only on explicit test
// entry: SD writes and readback never enter the measured gameplay windows.
void run_gpu_self_test(Display3ds&,const std::string& folder,bool restore_wide);
void write_game_pixel_comparison(const std::string& folder,
    const render::Framebuffer& reference,const render::Palette256&,
    std::span<const std::uint8_t> actual,bool wide,const char* backend,double alpha);
}
