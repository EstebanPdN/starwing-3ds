#include "../platform/3ds/source/frame_3ds.hpp"
#include "../platform/3ds/source/stereo_3ds.hpp"
#include "starfox/render/stereo_projection.hpp"
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <source_location>
#include <stdexcept>

using namespace starfox;
namespace {
void require(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition) throw std::runtime_error("3DS stereo/raster assertion at line "
        + std::to_string(where.line()));
}
int first_pixel(const render::Framebuffer& frame) {
    for (unsigned x = 0; x < frame.width(); ++x)
        for (unsigned y = 0; y < frame.height(); ++y)
            if (frame.get(x, y)) return int(x);
    throw std::runtime_error("empty stereo fixture");
}
void check_projection() {
    using namespace platform_3ds;
    require(stereo_slider_strength(0) == 0);
    require(stereo_slider_strength(-1) == 0);
    require(stereo_slider_strength(std::numeric_limits<float>::quiet_NaN()) == 0);
    require(stereo_slider_strength(.001F) == 1);
    require(stereo_slider_strength(.5F) == 4);
    require(stereo_slider_strength(2) == 8);
    require(stereo_bg2_quads_fit(8191) && !stereo_bg2_quads_fit(8192));
    assets::Shape quad;
    quad.vertices = {{-32,-24,0},{32,-24,0},{32,24,0},{-32,24,0}};
    assets::Face face; face.visibility_index = -1; face.vertex_indices = {3,2,1,0};
    quad.faces.push_back(face);
    render::SoftwareRenderer renderer;
    // Test the real polygon projection in both native-word and continuous paths.
    for (bool continuous : {false, true}) for (double depth : {128.,256.,512.,1024.,4096.}) {
        render::RenderPose pose; pose.z = depth; pose.use_rotation_matrix = true;
        pose.rotation_matrix = {32767,0,0,0,32767,0,0,0,32767};
        pose.palette_override = 203; pose.vanish_x = 200; pose.vanish_y = 120;
        pose.continuous_geometry = continuous; pose.subpixel_projection = continuous;
        render::Framebuffer mono(400,240), left(400,240), right(400,240);
        render::RenderDiagnostics diagnostics;
        renderer.draw(quad, pose, mono, true, nullptr, nullptr, &diagnostics);
        auto eye_pose = pose; apply_stereo_eye(eye_pose,-8); renderer.draw(quad, eye_pose, left);
        eye_pose = pose; apply_stereo_eye(eye_pose,8); renderer.draw(quad, eye_pose, right);
        if (std::none_of(mono.pixels().begin(),mono.pixels().end(),[](auto p){return p != 0;})) {
            for (const auto& polygon : diagnostics.polygons) {
                std::cerr << "area=" << polygon.signed_area << " projected:";
                for (const auto& point : polygon.projected) std::cerr << ' ' << point[0] << ',' << point[1];
                std::cerr << '\n';
            }
            throw std::runtime_error("empty mono quad continuous=" + std::to_string(continuous) + " depth=" + std::to_string(depth));
        }
        const int lx = first_pixel(left), mx = first_pixel(mono), rx = first_pixel(right);
        require(lx <= mx && mx <= rx); // left eye moves left: depth goes inside the screen
        require(mx - lx <= 8 && rx - mx <= 8);
        if (depth <= 256) require(left.pixels() == mono.pixels() && right.pixels() == mono.pixels());
        else require(lx < mx && mx < rx);
    }
}
void check_spans() {
    std::mt19937 random(670066);
    // Reference uses the original public pixel writer, including black writes,
    // clipped endpoints, absolute dither parity, coverage and layer ownership.
    for (unsigned trial = 0; trial < 12000; ++trial) {
        const unsigned width = 1U + random()%127U, height = 1U + random()%31U;
        render::Framebuffer actual(width,height), expected(width,height);
        const bool coverage = (trial & 1U), tags = (trial & 2U);
        actual.clear(77); expected.clear(77);
        if (coverage) { actual.begin_write_coverage(); expected.begin_write_coverage(); }
        actual.enable_layer_tags(tags); expected.enable_layer_tags(tags);
        if (trial & 4U) { actual.set_layer_override(render::PixelLayer::world_geometry);
            expected.set_layer_override(render::PixelLayer::world_geometry); }
        const int y = int(random()%(height+4))-2;
        const int left = int(random()%(width*3))-int(width);
        const int right = int(random()%(width*3))-int(width);
        const auto even = std::uint8_t(random()), odd = std::uint8_t(random());
        const bool dither = (trial & 8U);
        auto row = actual.raster_row(y);
        if (!row.fill_span(left,right,even,odd,dither))
            for (int x = left; x <= right; ++x) row.set(x, dither && ((x^y)&1) ? odd : even);
        for (int x = left; x <= right; ++x) expected.set(x,y, dither && ((x^y)&1) ? odd : even);
        require(actual.pixels() == expected.pixels());
        require(actual.layer_tags() == expected.layer_tags());
        require(std::equal(actual.write_coverage().begin(),actual.write_coverage().end(),expected.write_coverage().begin(),expected.write_coverage().end()));
    }
    render::Framebuffer scaled(8,4,2); require(!scaled.raster_row(1).fill_span(0,7,0,0,false));
    render::Framebuffer commands(8,4); render::RasterCommands stream;
    commands.record_to(&stream); require(!commands.raster_row(1).fill_span(0,7,0,0,false));
}
void check_frames(const char* rom_path, const char* symbols_path) {
    const auto rom = assets::RomImage::load(rom_path);
    const auto symbols = assets::SymbolMap::load(symbols_path);
    for (const char* scene : {"TITLEMAP", "LEVEL1_1"}) {
        const bool gameplay = std::string_view(scene) == "LEVEL1_1";
        auto game = std::make_unique<simulation::GameSimulation>(rom,symbols,
            gameplay ? "BOOT" : scene, std::span<const std::uint8_t>{}, true);
        // Use the application's level-select handoff: the low-level map
        // constructor intentionally bypasses parts of Original's boot state.
        if (gameplay) {
            game->set_selected_level(11);
            require(game->launch_selected_level());
        }
        game->set_timing_mode(simulation::TimingMode::unlocked_20_fps); game->set_presentation_fps(60);
        auto renderer = std::make_unique<platform_3ds::Frame3ds>(rom,symbols);
        unsigned paired = 0, different_eyes = 0;
        // Original starts LEVEL1_1 with a flat Mode 1 stage introduction.
        // Cover its handoff to actual Mode 2 geometry, not just that intro.
        for (unsigned frame = 0; frame < (gameplay ? 1200U : 180U); ++frame) {
            game->present_frame();
            if (game->logic_tick_ready()) static_cast<void>(game->tick({}));
            renderer->capture_after_tick(*game);
            for (bool gpu : {false,true}) {
                renderer->set_stereo_strength(0);
                const auto mono = renderer->draw(*game,gpu).pixels();
                require(!renderer->stereo_active());
                renderer->set_stereo_strength(8);
                require(!renderer->can_reuse_presentation(*game,gpu));
                const auto left = renderer->draw(*game,gpu).pixels();
                const auto right = renderer->right_frame().pixels();
                if (renderer->stereo_active()) {
                    ++paired; require(left.size() == 400U*240U && right.size() == left.size());
                    different_eyes += left != right;
                    // A repeated draw must retain both eyes of the same snapshot.
                    require(renderer->draw(*game,gpu).pixels() == left);
                    require(renderer->right_frame().pixels() == right);
                } else require(left == mono && right == mono);
                renderer->set_stereo_strength(0);
                require(renderer->draw(*game,gpu).pixels() == mono);
                require(renderer->right_frame().pixels() == mono);
            }
        }
        std::cout << scene << " paired_draws=" << paired << " different_eyes=" << different_eyes << '\n';
        if (!gameplay) require(paired == 0);
        else require(paired > 0 && different_eyes > 0);
    }
}
}
int main(int argc, char** argv) try {
    check_spans(); check_projection();
    if (argc == 3) check_frames(argv[1],argv[2]);
    std::cout << "Inward stereo, slider/cache transitions and 12000 raster differentials passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
