#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <utility>
#include <vector>

// Decoded Super FX model data: ShapeHdr fields, point frames, faces, BSP tree,
// colour words and textures. ShapeDecoder produces it; the software renderer,
// face materials and the 3DS frame builder read it. Addresses are 24-bit SNES
// addresses.
namespace starfox::assets {

struct Vec3i {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    friend bool operator==(const Vec3i&, const Vec3i&) = default;
};

// Point command that produced a block: signed 8- or 16-bit coordinates; the
// mirrored forms also emit an X-negated copy of every point.
enum class PointEncoding : std::uint8_t {
    signed8,
    signed16,
    mirrored_x_signed8,
    mirrored_x_signed16,
};

struct PointBlock {
    PointEncoding encoding{};
    std::vector<Vec3i> source_points;
};

// Geometry of one animation frame: point blocks in stream order and the
// expanded vertex list (mirrored copies included). word_coordinates[i] is true
// when vertex i came from a 16-bit command; the renderer applies
// ShapeHeader::shift only to 8-bit vertices.
struct ShapeFrame {
    std::vector<PointBlock> point_blocks;
    std::vector<Vec3i> vertices;
    std::vector<bool> word_coordinates;
};

// Three vertex indices from a Visibilities command. The renderer tests their
// projected winding to decide whether the faces and BSP nodes that reference
// this entry are visible.
struct Visibility {
    std::uint8_t a{};
    std::uint8_t b{};
    std::uint8_t c{};
};

// One polygon or sprite. visibility_index selects a Shape::visibilities entry
// (negative: no visibility test); colour_id indexes Shape::colour_words;
// normal is the stored signed 8-bit normal. Polygons list 2-12 vertices;
// sprites have one vertex and a raw size byte.
struct Face {
    std::int8_t visibility_index{};
    std::uint8_t colour_id{};
    Vec3i normal{};
    std::vector<std::uint8_t> vertex_indices;
    bool sprite{};
    std::uint8_t sprite_visibility_parameter{};
    std::uint8_t sprite_size{};
};

// Faces of one face list reached from the BSP tree, keyed by the list's
// address. The same faces are also appended to Shape::faces.
struct FaceBatch {
    std::uint32_t address{};
    std::vector<Face> faces;
};

// BSP split. visibility_index selects the Visibility that orders traversal;
// fallthrough_address is the next node in the stream and alternate_address
// the branch target (0 when absent).
struct BspNode {
    std::uint32_t address{};
    std::uint8_t visibility_index{};
    std::uint32_t face_batch_address{};
    std::uint32_t fallthrough_address{};
    std::uint32_t alternate_address{};
};

// BSP exit with its face batch, or a terminal node (BSP end, end of shape or
// quit) with face_batch_address 0.
struct BspLeaf {
    std::uint32_t address{};
    std::uint32_t face_batch_address{};
};

// A colour-table word (raw) and, when bits 15-14 are 10, the COLANIM list it
// references.
struct ColourMaterial {
    std::uint16_t raw{};
    // COLANIM points at a count byte followed by this many material words.
    // Keeping the original words preserves normal, diffuse and texture
    // frames without baking a renderer-specific interpretation into assets.
    std::vector<std::uint16_t> animation_frames;
};

struct TextureCoordinate {
    std::uint8_t u{};
    std::uint8_t v{};
};

// Texture named by a colour word whose bits 15-14 are 01 (descriptor).
// address is the texel page in ROM; u_mask and v_mask are width - 1 and
// height - 1; coordinates are the four corner (u, v) pairs; texels holds
// 4-bit colour indices in row-major order.
struct TextureImage {
    std::uint16_t descriptor{};
    std::uint32_t address{};
    std::uint8_t u_mask{};
    std::uint8_t v_mask{};
    std::array<TextureCoordinate, 4> coordinates{};
    std::vector<std::uint8_t> texels;
};

// Diffuse shade bytes from SHADESTAB2_0..3, indexed
// [depth 0-3][light 0-11][intensity 0-9].
using DiffuseShadeTables = std::array<
    std::array<std::array<std::uint8_t, 10>, 12>, 4>;

// Decoded ShapeHdr. points_address and faces_address are 24-bit; the colour,
// shadow and LOD pointers are 16-bit offsets within a bank. 8-bit coordinates
// are scaled by 2^shift. compact marks a ShapeHdr_s LOD header (points, bank
// and faces only) whose other fields come from the parent; see
// ShapeDecoder::decode_lod.
struct ShapeHeader {
    std::uint32_t address{};
    bool compact{};
    std::uint32_t points_address{};
    std::uint32_t faces_address{};
    std::int16_t sort_z{};
    std::uint8_t shift{};
    std::uint16_t radius{};
    std::int16_t x_max{};
    std::int16_t y_max{};
    std::int16_t z_max{};
    std::int16_t size{};
    std::uint16_t colour_pointer{};
    std::uint16_t shadow_pointer{};
    std::uint16_t lod1_pointer{};
    std::uint16_t lod2_pointer{};
    std::uint16_t lod3_pointer{};
};

// A decoded model. frames holds one entry per usable animation frame
// (declared_frame_count may be larger); point_blocks, vertices and
// word_coordinates repeat frames[0]. faces holds every face, including those
// in BSP batches; colour_words and colour_materials are indexed by colour_id.
// NULLSHAPE and strategy-only ShapeHdr 0,0 headers decode with no geometry.
struct Shape {
    std::string name;
    ShapeHeader header;
    std::uint8_t declared_frame_count{1};
    std::vector<PointBlock> point_blocks;
    std::vector<Vec3i> vertices;
    std::vector<bool> word_coordinates;
    std::vector<ShapeFrame> frames;
    std::vector<Visibility> visibilities;
    std::vector<Face> faces;
    std::vector<FaceBatch> face_batches;
    std::vector<BspNode> bsp_nodes;
    std::vector<BspLeaf> bsp_leaves;
    // Sorted address-to-index tables are derived once during decoding. The
    // renderer traverses these immutable BSP records on every presentation.
    std::vector<std::pair<std::uint32_t, std::size_t>> bsp_node_lookup;
    std::vector<std::pair<std::uint32_t, std::size_t>> bsp_leaf_lookup;
    std::vector<std::pair<std::uint32_t, std::size_t>> bsp_batch_lookup;
    std::uint32_t bsp_root_address{};
    std::vector<std::uint16_t> colour_words;
    std::vector<ColourMaterial> colour_materials;
    std::vector<TextureImage> textures;
    DiffuseShadeTables diffuse_shade_tables{};
    bool has_diffuse_shade_tables{};
};

} // namespace starfox::assets
