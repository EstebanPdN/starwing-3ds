#pragma once
#include "starfox/render/software_renderer.hpp"
namespace starfox::render {
void replay_raster_commands(const RasterCommands&,Framebuffer&,SurfaceBuffer*,bool clear_target=true);
}
