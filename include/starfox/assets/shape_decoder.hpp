#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/assets/shape.hpp"

#include <cstdint>
#include <string>

// Decodes cartridge models (ShapeHdr and its point, face, BSP, colour and
// texture data) from the ROM into assets::Shape. Used by the 3DS frame builder,
// host tools and tests.
namespace starfox::assets {

// Keeps a reference to rom, which must outlive the decoder. The SymbolMap
// constructor locates the texture tables, colour-table bank, diffuse shade
// tables and NULLSHAPE, and detects Star Fox EX's header layout; without
// symbols, shapes decode with no textures or shade tables.
class ShapeDecoder {
public:
    explicit ShapeDecoder(const RomImage& rom);
    ShapeDecoder(const RomImage& rom, const SymbolMap& symbols);

    // Decodes the ShapeHdr at header_address. A nonzero colour_pointer_override
    // replaces the header's colour table; minimum_colour_count reads that many
    // colour words even if no face uses them (above 256 throws
    // std::invalid_argument). Throws when the address does not hold a
    // supported shape.
    [[nodiscard]] Shape decode(
        std::uint32_t header_address,
        std::string name = {},
        std::uint16_t colour_pointer_override = 0,
        std::uint16_t minimum_colour_count = 0) const;
    // Decodes the LOD or shadow header at header_pointer in parent's bank; a
    // pointer to parent itself is a plain decode(). Otherwise the result takes
    // parent's shift and the override or parent colour table, and compact
    // headers also inherit sort_z, radius, extents and size from parent.
    [[nodiscard]] Shape decode_lod(
        const ShapeHeader& parent,
        std::uint16_t header_pointer,
        std::uint16_t colour_pointer_override = 0,
        std::uint16_t minimum_colour_count = 0) const;
    // Header pointer for view-space depth camera_z: the base header below
    // 1000, then lod1, lod2 and lod3 from 1000, 2000 and 3000.
    [[nodiscard]] static std::uint16_t select_lod_pointer(
        const ShapeHeader& header, double camera_z) noexcept;
    // Returns the first of the symbol's addresses that decodes; otherwise
    // throws std::runtime_error listing every failure.
    [[nodiscard]] Shape decode_by_name(const SymbolMap& symbols, const std::string& name) const;
    // True when the ShapeHdr at address passes header validation; its point
    // and face streams are not decoded.
    [[nodiscard]] bool looks_like_shape_header(std::uint32_t address) const noexcept;

private:
    [[nodiscard]] ShapeHeader decode_header(std::uint32_t address) const;
    void decode_points(Shape& shape) const;
    void decode_faces(Shape& shape) const;
    void decode_colours(Shape& shape, std::uint16_t minimum_count) const;
    void decode_texture(Shape& shape, std::uint16_t descriptor) const;

    const RomImage& rom_;
    std::uint32_t null_shape_address_{};
    std::uint32_t texture_address_table_{};
    std::uint32_t texture_coordinate_table_{};
    std::uint8_t colour_table_bank_{0x03U};
    std::uint16_t default_colour_pointer_{0x8213U};
    DiffuseShadeTables diffuse_shade_tables_{};
    bool has_diffuse_shade_tables_{};
    bool has_lod_pointers_{true};
};

} // namespace starfox::assets
