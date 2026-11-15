#pragma once
#include "starfox/assets/shape.hpp"
#include <array>
#include <cstddef>
#include <optional>
// Material resolution for one model face: a dithered colour pair or a texture
// from the shape's colour table. Used by the software renderer and host tests.
namespace starfox::render {
struct RenderPose;
// Colour pair as offsets from the renderer's colour_index_base; with dither
// set, pixels alternate between even and odd by X/Y parity.
struct FaceColour {std::uint8_t even{},odd{};bool dither{};};
// Texture references remain owned by Shape; neither raster backend may
// replace a texture or a dithered material with an averaged flat color.
struct FaceMaterial {FaceColour colour{};const assets::TextureImage* texture{};};
// The texture a colour-table entry names, following its animation frames by
// the frame argument (modulo their count); nullptr when the entry is out of
// range or matches no texture of the shape.
const assets::TextureImage* texture_for_colour(const assets::Shape&,std::uint8_t,std::uint32_t);
// Resolves a face's material in priority order: pose.palette_override, then
// pose.force_colour, then the face's colour word animated by colour_frame.
// descriptor_override (a COLOR WARP word) replaces that word as it is. Shaded
// materials use depth_band (clamped to 3) and the face normal against `light`.
FaceMaterial face_material(const assets::Shape&,const assets::Face&,std::uint32_t colour_frame,
    std::size_t depth_band,const std::array<std::int8_t,3>& light,const RenderPose&,
    std::optional<std::uint16_t> descriptor_override=std::nullopt,std::uint8_t colour_index_base=0);
}
