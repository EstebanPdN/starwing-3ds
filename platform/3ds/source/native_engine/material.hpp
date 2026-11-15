#pragma once
#include "scene.hpp"
#include "starfox/render/palette.hpp"
#include <algorithm>
#include <array>
// Encoding of the native engine's dithered rectangles for pica.cpp: the even
// colour is the vertex colour, the odd colour a texel of a 16x16 palette
// texture, and an 8x8 checker texture picks between them per pixel.
// Host-tested by tests/native_material_test.cpp and tests/native_pica_test.cpp.

namespace starfox::platform_3ds::native_engine {
// Texel offset of palette entry i (column i & 15, row i >> 4) in the 16x16
// palette texture's tiled layout: 8x8 Morton-ordered tiles.
constexpr unsigned palette_texel_offset(unsigned i) noexcept {
    const unsigned x=i&15U,y=i>>4;
    return (x&1U)|((y&1U)<<1)|((x&2U)<<1)|((y&2U)<<2)
        |((x&4U)<<2)|((y&4U)<<3)|((x&8U)<<3)|((y&8U)<<4);
}
inline render::Rgba8 material_display_colour(render::Rgba8 colour) noexcept {
    // Match the software presenter's RGB565 truncation before PICA's
    // interpolated primary colour and display-transfer quantization. Fades
    // can otherwise move a channel across a 5/6-bit boundary by one level.
    const unsigned r=colour.r>>3U,g=colour.g>>2U,b=colour.b>>3U;
    // Primary-colour interpolation on physical PICA can round a value on
    // the bottom boundary of a 565 bin down into the preceding bin. Use
    // its centre: the final 565 value stays identical, with rounding room.
    return {std::uint8_t((r<<3U)+4U),std::uint8_t((g<<2U)+2U),
        std::uint8_t((b<<3U)+4U),colour.a};
}
// Palette entry `index` as a Citro2D vertex colour (red in the low byte).
// Index 0 gets alpha 0 (transparent), here and in material_colour.
inline std::uint32_t material_vertex_colour(const render::Palette256& palette,std::uint8_t index) noexcept {
    const auto c=material_display_colour(palette[index]);
    return std::uint32_t(c.r)|(std::uint32_t(c.g)<<8)|(std::uint32_t(c.b)<<16)|(index?0xff000000U:0U);
}
// The same colour as an RGBA8 texel word (red in the high byte).
inline std::uint32_t material_colour(const render::Palette256& palette,std::uint8_t index) noexcept {
    const auto c=material_display_colour(palette[index]);
    return (std::uint32_t(c.r)<<24)|(std::uint32_t(c.g)<<16)|(std::uint32_t(c.b)<<8)|(index?255U:0U);
}
// Fills one 8x8 Morton-ordered tile (64 words) with a checkerboard: `even`
// where x + y is even, `odd` elsewhere.
inline void write_material(std::uint32_t* pixels,std::uint32_t even,std::uint32_t odd) noexcept {
    // Morton index bit 0 is x parity, bit 1 is y parity.
    for (unsigned i=0;i<64;++i) pixels[i]=((i^(i>>1))&1U)?odd:even;
}
// Palette-texture coordinates of an odd colour: the column from its low
// nibble, the row from its high nibble. Every entry is an exact binary
// fraction, so the table holds the very floats the expressions yield.
struct MaterialPaletteCoordinates {
    std::array<float,16> u{},v{};
};
constexpr MaterialPaletteCoordinates material_palette_coordinates() noexcept {
    MaterialPaletteCoordinates table;
    for (unsigned n=0;n<16U;++n) {
        table.u[n]=(n+0.5F)/16.0F;
        table.v[n]=1.0F-(n+0.5F)/16.0F;
    }
    return table;
}
// The four batch vertices of one rectangle, written in place. `Vertex` has
// the members of C2D_Vertex: pos[3], texcoord[2], ptcoord[2], color. Each
// float equals what material_quad() computes for it, converted vertex by
// vertex: a division by 16 is the same exact scaling as a product by 1/16,
// 1/(8*1) is exactly 1/8, and the palette coordinates come from the table
// above. Nothing is divided per rectangle and no intermediate quad is built
// or copied. `colour` is the even colour (material_vertex_colour).
template<class Vertex>
inline void write_material_quad(const Rectangle& r,std::uint32_t colour,
    const MaterialPaletteCoordinates& palette,Vertex* out) noexcept {
    float left,right,bottom_left,bottom_right,top,bottom;
    if (r.slanted) {
        left=r.top_left*0.0625F; right=r.top_right*0.0625F;
        bottom_left=r.bottom_left*0.0625F; bottom_right=r.bottom_right*0.0625F;
        top=r.top_y*0.0625F; bottom=r.bottom_y*0.0625F;
    } else {
        left=bottom_left=float(r.left); right=bottom_right=float(r.right);
        top=float(r.top); bottom=float(r.bottom);
    }
    const float factor=r.scale<=1U?0.125F:1.0F/(8.0F*r.scale);
    const float u=palette.u[r.odd&15U],v=palette.v[r.odd>>4];
    const float top_v=32.0F-top*factor,bottom_v=32.0F-bottom*factor;
    const auto write=[&](Vertex& vertex,float x,float y,float checker_v) {
        vertex.pos[0]=x; vertex.pos[1]=y; vertex.pos[2]=0.0F;
        vertex.texcoord[0]=u; vertex.texcoord[1]=v;
        vertex.ptcoord[0]=x*factor; vertex.ptcoord[1]=checker_v;
        vertex.color=colour;
    };
    write(out[0],left,top,top_v);
    write(out[1],right,top,top_v);
    write(out[2],bottom_left,bottom,bottom_v);
    write(out[3],bottom_right,bottom,bottom_v);
}
// A rectangle's corners (top-left, top-right, bottom-left, bottom-right) in
// pixels with their checker coordinates, by plain division: the reference
// that tests/native_material_test.cpp checks.
struct MaterialVertex { float x,y,u,v; };
inline std::array<MaterialVertex,4> material_quad(const Rectangle& r) noexcept {
    const float left=r.slanted?r.top_left/16.0F:float(r.left);
    const float right=r.slanted?r.top_right/16.0F:float(r.right);
    const float bl=r.slanted?r.bottom_left/16.0F:left;
    const float br=r.slanted?r.bottom_right/16.0F:right;
    const float top=r.slanted?r.top_y/16.0F:float(r.top);
    const float bottom=r.slanted?r.bottom_y/16.0F:float(r.bottom);
    const float factor=1.0F/(8.0F*std::max<unsigned>(1,r.scale));
    const auto v=[&](float x,float y) { return MaterialVertex{x,y,x*factor,32.0F-y*factor}; };
    return {v(left,top),v(right,top),v(bl,bottom),v(br,bottom)};
}
}
