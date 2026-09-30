#include "../platform/3ds/source/native_canvas.hpp"
#include "starfox/render/dust_renderer.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    using starfox::platform_3ds::NativeCanvasStyle;
    using starfox::platform_3ds::compose_native_canvas;
    using starfox::platform_3ds::overlay_source_coverage_on_native;
    starfox::render::Framebuffer source{256U, 224U};
    starfox::render::Framebuffer world{400U, 240U};
    starfox::render::Framebuffer native{400U, 240U};
    starfox::simulation::SnesPpuState ppu;
    ppu.cgram[7U] = 0x7fffU;

    source.clear(5U);
    source.set(100, 70, 12U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::controls);
    assert(native.get(0, 100) == 5U);
    assert(native.get(399, 100) == 5U);
    assert(native.get(132, 78) == 12U);
    assert(native.get(131, 78) == 5U);
    assert(native.get(132, 77) == 5U);
    assert(native.get(132, 79) == 5U);
    assert(native.get(172, 78) == 5U);
    assert(native.get(200, 0) == 5U);
    assert(native.get(200, 239) == 5U);

    world.clear(0U);
    world.set(35, 100, 12U);
    world.set(370, 100, 12U);
    world.set(200, 100, 12U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::gameplay);
    assert(native.get(35, 100) == 12U);
    assert(native.get(370, 100) == 12U);
    assert(native.get(200, 100) == 5U);
    assert(native.get(172, 78) == 12U);

    std::array<std::uint8_t, 224U> background_edge{};
    background_edge.fill(5U);
    for (unsigned y = 96U; y < 104U; ++y)
        source.set(0, static_cast<int>(y), 7U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::gameplay, background_edge, background_edge);
    assert(native.get(0, 108) == 5U);

    for (unsigned y = 216U; y < 224U; ++y) {
        background_edge[y] = 6U;
        source.set(0, static_cast<int>(y), 6U);
        source.set(255, static_cast<int>(y), 6U);
    }
    world.clear(0U);
    world.set(200, 4, 12U);
    world.set(200, 238, 12U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::gameplay, background_edge, background_edge);
    assert(native.get(200, 4) == 12U);
    assert(native.get(200, 238) == 12U);
    assert(native.get(180, 238) == 6U);
    assert(native.get(71, 238) == 6U);
    assert(native.get(328, 238) == 6U);

    // Intro margins display the live world projection. No independent static
    // star pattern may remain when the projected dust moves to a new row.
    source.clear(0U);
    source.set(100,70,12U);
    world.clear(0U);
    world.set(35,100,12U);
    world.set(370,100,12U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::intro);
    assert(native.get(35,100)==12U);
    assert(native.get(370,100)==12U);
    assert(native.get(172,78)==12U);
    world.clear(0U);
    world.set(35,99,12U);
    world.set(370,99,12U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::intro);
    assert(native.get(35,99)==12U);
    assert(native.get(370,99)==12U);
    assert(native.get(35,100)==0U);
    assert(native.get(370,100)==0U);
    world.clear(0U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::intro);
    const auto unrotated_stars=native.pixels();
    unsigned side_star_count=0U;
    for(unsigned y=8U;y<232U;++y) for(unsigned x=0U;x<400U;++x) {
        if(x>=72U&&x<328U) continue;
        if(native.get(x,y)!=0U) ++side_star_count;
    }
    assert(side_star_count>=40U&&side_star_count<=80U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::intro,{}, {},-3);
    for(unsigned y=8U;y<232U;++y) for(unsigned x=0U;x<400U;++x) {
        if(x>=72U&&x<328U) continue;
        const auto source_y=8U+(y-8U+3U)%224U;
        assert(native.get(x,y)==unrotated_stars[source_y*400U+x]);
    }

    // The side copies use the same projected dust points as the centre.
    // Rotating the view must move both sides vertically in the same frame.
    starfox::render::DustRenderer::DustFrame dust_frame;
    dust_frame.points = {{100,0,1024},{-100,0,1024}};
    dust_frame.colours.fill(1U);
    dust_frame.matrix = {32767,0,0,0,32767,0,0,0,32767};
    const auto draw_intro_dust = [&] {
        world.clear(0U);
        dust_frame.offset_x = 0;
        dust_frame.exclude_left = dust_frame.exclude_right = 0;
        starfox::render::DustRenderer::draw_dust_frame(dust_frame,world);
        dust_frame.offset_x = -192;
        dust_frame.exclude_left = 72;
        dust_frame.exclude_right = 400;
        starfox::render::DustRenderer::draw_dust_frame(dust_frame,world);
        dust_frame.offset_x = 192;
        dust_frame.exclude_left = 0;
        dust_frame.exclude_right = 328;
        starfox::render::DustRenderer::draw_dust_frame(dust_frame,world);
    };
    draw_intro_dust();
    const auto side_y = [&](int first_x,int last_x) {
        for(int y=0;y<240;++y) for(int x=first_x;x<last_x;++x)
            if(world.get(x,y)!=0U) return y;
        return -1;
    };
    assert(side_y(0,72)==120);
    assert(side_y(328,400)==120);
    dust_frame.matrix = {32767,0,0,0,32269,5690,0,-5690,32269};
    draw_intro_dust();
    const int left_y=side_y(0,72),right_y=side_y(328,400);
    assert(left_y>=0 && left_y==right_y && left_y!=120);

    // Match the source map's sage-gray five-pixel crosses and single dots.
    // A wrapped neon-green eight-pixel row in the source's empty top-left
    // field is removed only on the star map.
    ppu.cgram[9U] = 16U | (18U << 5U) | (16U << 10U);
    ppu.cgram[10U] = 23U | (25U << 5U) | (23U << 10U);
    ppu.cgram[11U] = 5U | (31U << 5U);
    source.clear(5U);
    for (int x = 0; x < 8; ++x) source.set(x, 7, 11U);
    world.clear(0U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::star_map);
    for (int x = 72; x < 80; ++x) assert(native.get(x, 15) == 5U);
    assert(native.get(25, 53) == 10U);
    assert(native.get(24, 53) == 9U);
    assert(native.get(26, 53) == 9U);
    assert(native.get(25, 52) == 9U);
    assert(native.get(25, 54) == 9U);
    assert(native.get(25, 51) == 5U);
    assert(native.get(12, 19) == 9U);
    assert(native.get(365, 55) == 10U);
    for (unsigned y = 0U; y < 240U; ++y) {
        for (unsigned x = 0U; x < 400U; ++x) {
            if (x >= 72U && x < 328U) continue;
            assert(native.get(x, y) != 7U);
            assert(native.get(x, y) != 11U);
        }
    }

    // Game Over extends only the empty native side columns with isolated
    // coloured pixels; the cartridge's middle sky remains at its source size.
    source.clear(0U);
    source.set(100,70,12U);
    ppu.cgram[13U] = 28U | (11U << 5U) | (10U << 10U);
    ppu.cgram[14U] = 29U | (22U << 5U) | (9U << 10U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::game_over);
    unsigned game_over_left=0U,game_over_right=0U;
    for(unsigned y=0U;y<240U;++y) for(unsigned x=0U;x<400U;++x) {
        if(x<72U&&native.get(x,y)!=0U) ++game_over_left;
        if(x>=328U&&native.get(x,y)!=0U) ++game_over_right;
    }
    // The added columns match the cartridge's approximately 1:420 sky.
    assert(game_over_left>=34U&&game_over_left<=48U);
    assert(game_over_right>=34U&&game_over_right<=48U);
    assert(native.get(13,14) == 0U);
    assert(native.get(172,78) == 12U);

    source.clear(0U);
    source.begin_write_coverage();
    source.set(20, 30, 0U); // An authored black pixel is still foreground.
    source.set(21, 30, 5U); // Same colour still has explicit ownership.
    source.end_write_coverage();
    native.clear(5U);
    native.begin_write_coverage();
    native.set(200, 100, 12U); // Model pixel in the central source square.
    overlay_source_coverage_on_native(source, native);
    assert(native.get(200, 100) == 12U);
    assert(native.get(92, 38) == 0U);
    assert(native.get(93, 38) == 5U);
    assert(native.write_coverage()[38U * 400U + 92U] == 1U);
    assert(native.write_coverage()[38U * 400U + 93U] == 1U);
    native.end_write_coverage();

    world.clear(0U);
    source.clear(0U);
    compose_native_canvas(source, world, native, ppu,
        NativeCanvasStyle::star_map);
    assert(std::all_of(native.pixels().begin(), native.pixels().end(),
        [](std::uint8_t pixel) { return pixel == 0U; }));
    std::puts("Native 400x240 canvas: source pixels remain 1:1, margins and world pass");
}
