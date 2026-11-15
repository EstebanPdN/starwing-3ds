#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/render/software_renderer.hpp"

#include <cstdint>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// Text from the cartridge's own fonts and strings (MDSPRITE.MC projected text,
// MTXTPRT.MC game text, FACEDATA portraits) plus localized host menu text.
// Used by the 3DS frame code and options menu, host tools and tests.
namespace starfox::render {

// CGRAM base (palette 6) of the briefing's game text.
inline constexpr std::uint8_t briefing_text_palette_base = 6U * 16U;

// Renderer for MDSPRITE.MC's 16x16 projected text objects. Strings and glyph
// rows are consumed directly from the assembled ROM rather than substituted
// with a host font.
class ScaledTextRenderer {
public:
    // A projected text object captured for deferred (per-eye) drawing: glyph
    // rows, pose, size in world units and final colour index.
    struct ProjectedFrame {
        std::vector<std::array<std::uint16_t,16>> glyphs;
        RenderPose pose{};
        std::int32_t character_size{};
        std::uint8_t colour{};
    };
    // Reads the zero-terminated message at message_pointer (in MARIOMSGS'
    // bank) and its MSCALECHARS glyphs; character_size is 127 plus
    // size_adjustment. Empty when pose.z < 128 or the pointer is below $8000.
    [[nodiscard]] ProjectedFrame prepare_projected(std::uint16_t message_pointer,
        std::uint8_t colour,std::int8_t size_adjustment,const RenderPose&,
        std::uint8_t colour_index_base=7U*16U) const;
    // Draws a captured object: square glyphs scaled by 256/z around the
    // projected pose, rasterized at stored resolution.
    static void draw_projected(const ProjectedFrame&,Framebuffer&);
    // English (Europe), ID 5, shares the original English text/font path, as
    // does any other ID above 4; IDs 1-4 select the translated catalogs.
    void set_language(std::uint8_t language) noexcept { language_ = language < 5 ? language : 0; }
    // Resolves the font, message and portrait symbols in `rom`, which must
    // outlive the renderer; throws std::runtime_error when a required one is
    // missing. FACEDATA2 (Star Fox EX) is optional.
    ScaledTextRenderer(
        const assets::RomImage& rom,
        const assets::SymbolMap& symbols);

    // prepare_projected(), then draw_projected().
    void draw(
        std::uint16_t message_pointer,
        std::uint8_t colour,
        std::int8_t size_adjustment,
        const RenderPose& pose,
        Framebuffer& target,
        std::uint8_t colour_index_base = 7U * 16U) const;

    // Draw MTXTPRT.MC's variable-width 12-pixel game text directly from a
    // source `txt` record (colour byte, ASCII bytes, zero terminator).
    // Lines wrap on spaces at right_clip, 13 pixels apart, for at most
    // max_characters; forced_colour replaces the record's colour. With a
    // language selected, a translated dialogue replaces the record. An
    // address in the lower half of its bank draws nothing.
    void draw_game_text(
        std::uint32_t text_address,
        std::int32_t x,
        std::int32_t y,
        Framebuffer& target,
        std::uint8_t colour_index_base = 7U * 16U,
        std::optional<std::uint8_t> forced_colour = std::nullopt,
        std::int32_t right_clip = 224,
        std::size_t max_characters = 256U) const;

    // Draw one of FACEDATA's original 32x40, 4-bpp portrait frames.
    // alternate_portraits selects FACEDATA2 when the ROM has it;
    // correct_pixel_aspect widens the portrait to the SNES 7:6 pixel aspect,
    // keeping its right edge.
    void draw_face(
        std::uint8_t frame,
        std::int32_t x,
        std::int32_t y,
        Framebuffer& target,
        std::uint8_t colour_index_base = 7U * 16U,
        bool alternate_portraits = false,
        bool correct_pixel_aspect = false) const;

    // Host-authored ASCII in the cartridge's 12-pixel game font (':' and '/'
    // are drawn by hand). With a language selected it draws the menu
    // translation, or the text itself, through draw_unicode() at menu size.
    void draw_ascii(
        std::string_view text,
        std::int32_t x,
        std::int32_t y,
        Framebuffer& target,
        std::uint8_t colour = 14U,
        std::uint8_t colour_index_base = 7U * 16U) const;

    // Preserve the source font's horizontal metrics while reducing its
    // twelve scanlines to eight for dense host-authored option lists.
    void draw_ascii_compact(
        std::string_view text,
        std::int32_t x,
        std::int32_t y,
        Framebuffer& target,
        std::uint8_t colour = 14U,
        std::uint8_t colour_index_base = 7U * 16U) const;

    // Width of the widest line draw_ascii() would draw.
    [[nodiscard]] std::int32_t measure_ascii(std::string_view text) const;
    // Localized text. At menu_size, Latin letters (accents included) use the
    // cartridge font and other glyphs the bitmap font scaled to 12 pixels;
    // otherwise every glyph is an 8-pixel bitmap glyph. Missing glyphs draw
    // '?'.
    void draw_unicode(std::u32string_view text, std::int32_t x, std::int32_t y,
        Framebuffer& target, std::uint8_t colour = 14U,
        std::uint8_t colour_index_base = 7U * 16U, bool menu_size = false) const;
    // Widest line at the 8-pixel bitmap advances (not menu_size).
    [[nodiscard]] std::int32_t measure_unicode(std::u32string_view text) const;
    // The translation of the game text at `address`, wrapped to `width`
    // pixels with measure_unicode(); empty without a language or translation.
    [[nodiscard]] std::vector<std::u32string_view> translated_game_text_lines(
        std::uint32_t address, std::int32_t width, std::size_t max_characters = 256U) const;

private:
    [[nodiscard]] std::int32_t menu_glyph_advance(char32_t code) const;
    [[nodiscard]] std::int32_t measure_menu_unicode(std::u32string_view text) const;
    std::uint8_t language_{};
    std::unordered_map<std::uint32_t, unsigned> dialogue_ids_;
    const assets::RomImage* rom_{};
    std::uint32_t font_{};
    std::uint32_t messages_{};
    std::uint32_t game_font_widths_{};
    std::uint32_t game_font_glyphs_{};
    std::uint32_t game_font_translation_{};
    std::uint32_t face_data_{};
    std::uint32_t face_data_2_{};
};

} // namespace starfox::render
