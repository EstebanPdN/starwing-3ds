#pragma once
#include "starfox/render/software_renderer.hpp"
// Software reference replay of RasterCommands. Used by the 3DS frame code and
// host tests.
namespace starfox::render {
// Draws the batch's `commands` (a packed recorder must unpack() first) into
// the frame's stored pixels with each command's tag, and into the surfaces
// when given. Detaches any recorder from the frame; clear_target first clears
// the frame and surfaces.
void replay_raster_commands(const RasterCommands&,Framebuffer&,SurfaceBuffer*,bool clear_target=true);
}
