#include "starfox/render/software_renderer.hpp"
#include "starfox/render/raster_commands.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace starfox;

int main() try {
    std::uint32_t random_state = 0x3658faceU;
    const auto random = [&] { random_state = random_state * 1664525U + 1013904223U; return random_state; };
    std::uint64_t digest = 0xcbf29ce484222325ULL, commands = 0, coloured = 0;
    const auto hash = [&](const auto& values) {
        for (auto v : std::as_bytes(std::span{values})) {
            digest ^= std::uint8_t(v); digest *= 0x100000001b3ULL;
        }
    };
    render::software_renderer_detail::Scratch scratch;
    for (unsigned n = 0; n < 8192U; ++n) {
        assets::Shape shape;
        shape.header.shift = n % 8;
        shape.vertices = {{-90,-60,0},{-80,60,25},{90,65,0},{80,-65,-20}};
        for (auto& v : shape.vertices) {
            const auto extent = n % 3 == 0 ? 32768 : 128;
            v.x += int(random() % (2 * extent)) - extent;
            v.y += int(random() % (2 * extent)) - extent;
            v.z += int(random() % (2 * extent)) - extent;
        }
        shape.word_coordinates = {n % 2 == 0, n % 3 == 0, n % 5 == 0, n % 7 == 0};
        shape.colour_words = {0x006c, 0x0009, 0x4000};
        shape.visibilities = {{0,1,2},{2,1,0}};
        shape.faces = {{-1,0,{0,0,127},{0,1,2,3}}, {-1,1,{0,0,127},{3,2,1,0}}};
        if (n % 5 == 0) { shape.faces[0].visibility_index = 0; shape.faces[1].visibility_index = 1; }
        if (n % 11 == 0) shape.faces[0].vertex_indices = {0,2};
        if (n % 13 == 0) shape.faces[0].vertex_indices.push_back(99); // invalid geometry
        if (n % 17 == 0) {
            assets::TextureImage texture;
            texture.descriptor = 0x4000; texture.u_mask = texture.v_mask = 7;
            texture.coordinates = {{{0,0},{0,7},{7,7},{7,0}}};
            for (unsigned i=0; i<64; ++i) texture.texels.push_back(i%7);
            shape.textures.push_back(texture); shape.faces[0].colour_id=2;
        }
        render::RenderPose pose;
        pose.use_rotation_matrix = true;
        pose.rotation_matrix = {32767,0,0,0,32767,0,0,0,32767};
        if (n % 2) for (auto& v : pose.rotation_matrix) v = std::int16_t(random());
        pose.x = std::int16_t(random()); pose.y = std::int16_t(random());
        constexpr std::int16_t depths[]{-32768,-16384,-1,0,1,2,63,64,255,256,4096,32767};
        pose.z = depths[n % std::size(depths)];
        pose.vanish_x = n % 3 ? 200 : std::int16_t(random());
        pose.vanish_y = n % 3 ? 120 : std::int16_t(random());
        pose.scale = n % 19 == 0 ? 0.3 : 1.0;
        pose.explosion_progress = n % 23 == 0 ? 7 : 0;
        pose.wireframe_mode = n % 29 == 0 ? 1 : 0;
        pose.wave_mode = n % 31 == 0;
        pose.wobble_mode = n % 37 == 0 ? 2 : 0;
        pose.cel_mode = n % 41 == 0;
        pose.subpixel_projection = n % 43 == 0;
        pose.stereo_eye_offset = n % 47 == 0 ? -8 : 0;
        render::RenderSettings settings;
        settings.backface_culling = n % 53 == 0;
        settings.focal_length = n % 59 == 0 ? 255.5 : 256.0;
        settings.render_scale = n % 61 == 0 ? 2 : 1;
        const unsigned width = n % 2 ? 400 : 224, height = n % 2 ? 240 : 192;
        render::Framebuffer frame(width,height,settings.render_scale);
        frame.enable_layer_tags(true); frame.begin_write_coverage();
        render::RasterCommands batch;
        batch.reset(frame.stored_width(),frame.stored_height());
        batch.accept_edge_runs(n % 2);
        if (n % 4 == 0) frame.record_to(&batch);
        render::SoftwareRenderer{settings}.draw(shape,pose,frame,false,nullptr,nullptr,&scratch);
        frame.record_to(nullptr);
        hash(frame.pixels()); hash(frame.layer_tags()); hash(frame.write_coverage());
        hash(batch.commands); hash(batch.texels);
        commands += batch.commands.size();
        coloured += std::count_if(frame.pixels().begin(),frame.pixels().end(),[](auto v){return v!=0;});
    }
    std::printf("8192 poses: digest=%016llx commands=%llu coloured_pixels=%llu\n",
        (unsigned long long)digest,(unsigned long long)commands,(unsigned long long)coloured);
    // Golden produced by the independent pre-optimization renderer, with
    // byte-for-byte comparison of pixels, tags, coverage and raster commands.
    if (digest != 0xbbaf13ef72c0a4a6ULL || commands != 240499U || coloured != 30033975U)
        throw std::runtime_error("source-word renderer regression");
    return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
