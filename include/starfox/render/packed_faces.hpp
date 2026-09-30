#pragma once
#include "starfox/render/packed_bsp.hpp"
#include "starfox/render/raster_commands.hpp"
#include "starfox/render/software_renderer.hpp"
#include "starfox/render/gpu_colour_warp.hpp"
#include "starfox/render/packed_projection.hpp"
namespace starfox::render {
enum class PackedPrimitive : std::uint8_t { empty,polygon,line,sprite };
struct PackedFaces {
    std::vector<std::array<std::uint32_t,4>> corners,polygons;
    std::vector<RasterCommand> materials;
    std::vector<std::uint8_t> texels;
    std::vector<PackedPrimitive> primitives;
    bool polygon_only{true};
};
struct PackedWarpTextures {
    // Missing descriptors map to UINT32_MAX. Entries retain the source's
    // first matching texture when duplicate descriptors are present.
    std::vector<std::uint32_t> lookup;
    // texel offset, U mask, V mask, first coordinate (four per texture).
    std::vector<std::array<std::uint32_t,4>> textures;
    std::vector<std::array<std::uint32_t,2>> coordinates;
    std::vector<std::uint8_t> texels;
};
[[nodiscard]] PackedWarpTextures pack_warp_textures(const assets::Shape&);
struct PackedWarpShading {
    GpuWarpSettings settings;
    std::array<std::uint8_t,2480> diffuse{};
};
// Common CPU-authored inputs for SDL and resident Vulkan warp dispatch.
// Geometry counts/seed are filled by the caller after its topology is known.
[[nodiscard]] PackedWarpShading pack_warp_shading(const assets::Shape&,const RenderPose&,const RenderSettings&,bool axis=false);
// Arrays retain PackedBsp's face ID space. Polygons consume the unconditional
// visibility entry appended by pack_projection at shape.visibilities.size().
// Polygon, line and sprite-face emission are supported.
// Surface metadata is not generated here; normals/depth need a GPU stage.
// Destruction uses unconditional visibility; GpuModel supplies per-face offsets.
// Order-dependent colour warp and unsupported EX alternate emitters reject.
[[nodiscard]] PackedFaces pack_faces(const assets::Shape&,const PackedBsp&,const RenderPose&,const RenderSettings&);
// Material-free topology/templates for the ordered GPU colour-warp stages.
// Keeps untextured cel/wave/wireframe flags; descriptor decode chooses textures
// and their UVs later, independently for every painter-order occurrence.
[[nodiscard]] PackedFaces pack_warp_faces(const assets::Shape&,const PackedBsp&,const RenderPose&,const RenderSettings&);
// Expand authored corners into per-fragment GPU transform inputs. Vertex
// rotation/displacement still run on GPU; the returned poses share its ABI.
[[nodiscard]] std::vector<ContinuousTransformPose> pack_continuous_fragments(
    PackedProjection&,PackedFaces&,const PackedBsp&,const RenderPose&,const RenderSettings&,bool colour_warp);
}
