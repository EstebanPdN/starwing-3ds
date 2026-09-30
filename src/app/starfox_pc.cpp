#include "starfox/audio/spc700_audio.hpp"
#include "starfox/render/effects.hpp"
#include "starfox/render/bloom.hpp"
#include "starfox/render/object_snapshot.hpp"
#include "starfox/audio/msu1_audio.hpp"
#include "starfox/audio/stem_mixer.hpp"
#include "starfox/audio/msu1_pack.hpp"
#include "starfox/app/runtime_input.hpp"
#include "starfox/app/audio_queue.hpp"
#include "starfox/assets/bps.hpp"
#include "starfox/assets/embedded.hpp"
#include "starfox/assets/rom.hpp"
#include "starfox/assets/runtime_bundle.hpp"
#include "starfox/assets/shape_decoder.hpp"
#include "starfox/input/buttons.hpp"
#include "starfox/input/input_latch.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/render/background_renderer.hpp"
#include "starfox/render/pixel_filter.hpp"
#include "starfox/render/display_aspect.hpp"
#include "starfox/render/row_workers.hpp"
#include "starfox/render/dust_renderer.hpp"
#include "starfox/render/shadow_mask.hpp"
#include "starfox/render/dxr_shadows.hpp"
#include "starfox/render/portable_shadows.hpp"
#include "starfox/render/sdl_dxr_shadows.hpp"
#include "starfox/render/gpu_raster.hpp"
#include "starfox/render/gpu_scene.hpp"
#include "starfox/render/gpu_effects.hpp"
#include "starfox/render/sdl_gpu_effects.hpp"
#include "starfox/render/chromatic_aberration.hpp"
#include "starfox/render/hdr_effect.hpp"
#include "starfox/render/palette.hpp"
#include "starfox/render/particle_renderer.hpp"
#include "starfox/render/presentation_history.hpp"
#include "starfox/render/model_motion_history.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include "starfox/render/software_renderer.hpp"
#include "renderer_window.hpp"
#include "dlss_host.hpp"
#include "starfox/render/terrain_profile.hpp"
#include "starfox/render/sprite_renderer.hpp"
#include "starfox/render/colour_math.hpp"
#include "starfox/render/model_smoothing.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "starfox/simulation/math.hpp"
#include "starfox/timing/fixed_step.hpp"
#include "starfox/state/archive.hpp"
#include "starfox/state/container.hpp"
#include "starfox/state/files.hpp"

#include <SDL3/SDL.h>
#if defined(STARFOX_UWP)
#define SDL_MAIN_NOIMPL
#endif
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS) || defined(STARFOX_UWP)
#include <SDL3/SDL_main.h>
#ifdef main
#undef main
#endif
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32) && !defined(STARFOX_UWP)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#elif defined(__APPLE__) && !defined(SDL_PLATFORM_IOS)
#include <mach-o/dyld.h>
#endif

namespace {

using starfox::input::ButtonMask;

constexpr std::uint32_t snes_width = 256U;
constexpr std::uint32_t snes_height = 224U;
constexpr std::uint32_t widescreen_16_9_width = 400U;
constexpr std::uint32_t widescreen_16_10_width = 360U;
constexpr std::uint32_t ultrawide_width = 520U;
constexpr std::uint32_t super_ultrawide_width = 800U;
constexpr std::uint32_t superfx_height = 192U;
constexpr std::int32_t superfx_offset_y = 16;
constexpr std::uint32_t superfx_ui_width = 224U;

class FrameStepRepeater {
public:
    enum class Direction : std::uint8_t { forward, backward };
    using clock = std::chrono::steady_clock;

    void press(Direction direction, clock::time_point now) noexcept {
        direction_ = direction;
        repeat_at_ = now + std::chrono::milliseconds{450};
    }

    void release(Direction direction) noexcept {
        if (direction_ == direction) direction_.reset();
    }

    void reset() noexcept { direction_.reset(); }

    [[nodiscard]] std::optional<Direction> poll(
        clock::time_point now) noexcept {
        if (!direction_.has_value() || now < repeat_at_) return std::nullopt;
        constexpr auto interval = std::chrono::milliseconds{125};
        do {
            repeat_at_ += interval;
        } while (repeat_at_ <= now);
        return direction_;
    }

private:
    std::optional<Direction> direction_;
    clock::time_point repeat_at_{};
};

struct RuntimeAssets {
    starfox::assets::RomImage rom;
    starfox::assets::SymbolMap symbols;
};

struct ScriptedPress {
    std::uint64_t presentation_frame{};
    ButtonMask buttons{};
};

struct DeferredBackground {
    starfox::render::GpuSceneRecording scene;
    starfox::render::RasterCommands pending;
    std::shared_ptr<const starfox::simulation::SnesPpuState> ppu;
    starfox::render::PixelLayer tag{starfox::render::PixelLayer::background};
    starfox::render::PixelLayer base_tag{starfox::render::PixelLayer::background};
    unsigned margin_origin{};
    bool repair_margins{};
};

void repair_logo_margins(starfox::render::Framebuffer& frame,unsigned origin) {
    const auto colour=frame.get(origin,0);
    for(unsigned y=0;y<frame.height();++y) for(unsigned x=0;x<frame.width();++x)
        if((x<origin || x>=origin+256) && !frame.get(x,y)) frame.set(x,y,colour);
}

void fill_frontend_margins(starfox::render::Framebuffer& frame,unsigned origin) {
    const unsigned right=origin+256;
    const auto edge=[&](unsigned x) {
        std::array<unsigned,256> counts{};
        for(unsigned y=0;y<frame.height();++y) ++counts[frame.get(x,y)];
        return std::uint8_t(std::distance(counts.begin(),std::max_element(counts.begin(),counts.end())));
    };
    const auto left_colour=edge(origin),right_colour=edge(right-1);
    for(unsigned y=0;y<frame.height();++y) {
        for(unsigned x=0;x<origin;++x) frame.set(x,y,left_colour);
        for(unsigned x=right;x<frame.width();++x) frame.set(x,y,right_colour);
    }
}

// Keeps the cartridge's interleaved tile/OBJ painter order. Sprite writes
// enter the pending raster chunk; each tile pass flushes that chunk first.
class RecordingBackgroundRenderer : public starfox::render::BackgroundRenderer {
public:
    void draw_title_foreground(const starfox::simulation::SnesPpuState& ppu,int x,int y,
        starfox::render::Framebuffer& frame,int origin,bool include=true,bool extend=false) const {
        if(!recording || &frame!=target) {BackgroundRenderer::draw_title_foreground(ppu,x,y,frame,origin,include,extend);return;}
        draw_bg2(ppu,x,y,frame,starfox::render::TilePriorityPass::high,origin,extend,!extend,false);
        if(include) draw_bg1(ppu,frame,starfox::render::TilePriorityPass::all,origin,false,extend?16U:0U,false);
        draw_bg3(ppu,frame,starfox::render::TilePriorityPass::high,origin,false);
    }
    DeferredBackground* recording{};
    starfox::render::Framebuffer* target{};
    bool record(starfox::render::Framebuffer& frame,starfox::render::GpuBackgroundSettings settings) const {
        if(!recording || &frame!=target) return false;
        settings.tag=recording->tag;
        recording->scene.append_background(recording->pending,{recording->ppu,std::move(settings),frame.draw_scale()});
        return true;
    }
    void draw_bg1(const starfox::simulation::SnesPpuState& ppu,starfox::render::Framebuffer& frame,
        starfox::render::TilePriorityPass priority=starfox::render::TilePriorityPass::all,int origin=0,
        bool extend=true,unsigned inset=0,bool black=false) const {
        starfox::render::GpuBackgroundSettings s;s.layer=1;s.priority=priority;s.horizontal_origin=origin;
        s.extend_horizontal=extend;s.horizontal_inset=inset;s.transparent_cgram_black=black;
        if(!record(frame,s)) BackgroundRenderer::draw_bg1(ppu,frame,priority,origin,extend,inset,black);
    }
    void draw_bg2(const starfox::simulation::SnesPpuState& ppu,int x,int y,starfox::render::Framebuffer& frame,
        starfox::render::TilePriorityPass priority=starfox::render::TilePriorityPass::all,int origin=0,
        bool extend=true,bool wrap=true,bool black=false,unsigned rows=0,
        std::span<const starfox::render::BackgroundUniqueRegion> regions={}) const {
        starfox::render::GpuBackgroundSettings s;s.layer=2;s.priority=priority;s.horizontal_origin=origin;
        s.scroll_x=x;s.scroll_y=y;s.extend_horizontal=extend;s.wrap_horizontal=wrap;
        s.transparent_cgram_black=black;s.single_occurrence_top_rows=rows;s.unique_regions.assign(regions.begin(),regions.end());
        if(!record(frame,s)) BackgroundRenderer::draw_bg2(ppu,x,y,frame,priority,origin,extend,wrap,black,rows,regions);
    }
    void draw_bg3(const starfox::simulation::SnesPpuState& ppu,starfox::render::Framebuffer& frame,
        starfox::render::TilePriorityPass priority=starfox::render::TilePriorityPass::all,int origin=0,bool extend=true) const {
        starfox::render::GpuBackgroundSettings s;s.layer=3;s.priority=priority;s.horizontal_origin=origin;s.extend_horizontal=extend;
        if(!record(frame,s)) BackgroundRenderer::draw_bg3(ppu,frame,priority,origin,extend);
    }
};

// Restore the omitted background only where no intervening CPU write exists.
// Model replay and later foreground restoration remain in their original order.
void restore_background(const DeferredBackground& draw,
    starfox::render::Framebuffer& target,std::span<const std::uint8_t> coverage) {
    starfox::render::Framebuffer background(target.width(),target.height(),target.draw_scale());
    background.enable_layer_tags(true);
    background.begin_write_coverage();
    draw.scene.replay(background,nullptr);
    if(draw.margin_origin && draw.repair_margins) repair_logo_margins(background,draw.margin_origin);
    for(std::size_t i=0;i<target.pixels().size();++i) if(coverage.empty() || !coverage[i]) {
        target.pixels()[i]=background.pixels()[i];
        if(target.layer_tags_enabled()) target.layer_tags()[i]=background.write_coverage()[i]
            ?background.layer_tags()[i]:std::uint8_t(draw.base_tag);
    }
}

void apply_late_cartridge(const DeferredBackground& draw,starfox::render::Framebuffer& frame) {
    starfox::render::Framebuffer layer(frame.width(),frame.height(),frame.draw_scale());
    layer.enable_layer_tags(true);layer.begin_write_coverage();draw.scene.replay(layer,nullptr);
    for(unsigned y=0;y<frame.stored_height();++y) for(unsigned x=0;x<frame.stored_width();++x) {
        const auto i=std::size_t(y)*frame.stored_width()+x;
        if(layer.write_coverage()[i]) frame.set_stored(x,y,layer.pixels()[i],starfox::render::PixelLayer(layer.layer_tags()[i]));
    }
}

struct PresentationEffects {
    const DeferredBackground* late_cartridge{};
    const DeferredBackground* background{};
    std::span<const std::uint8_t> background_cpu_coverage;
    const starfox::render::Framebuffer* temporal_background{};
    starfox::render::TemporalCamera temporal_camera;
    std::optional<starfox::render::TemporalGroundPlane> temporal_ground;
    const starfox::render::DustRenderer::DustFrame* late_dust{};
    float late_dust_eye_x{};
    const starfox::render::Framebuffer* setup_overlay{};
    std::int32_t setup_left{12};
    std::int32_t setup_right{243};
    std::uint8_t setup_brightness{15U};
    const starfox::render::Framebuffer* overlay{};
    std::uint8_t overlay_brightness{30U};
    const starfox::render::Framebuffer* text_overlay{};
    std::uint8_t text_overlay_brightness{30U};
    const starfox::render::Framebuffer* host_overlay{};
    std::int32_t host_overlay_x{};
    std::int32_t host_overlay_y{};
    const starfox::render::Framebuffer* confirmation_overlay{};
    std::uint8_t background_fixed_white_subtract{};
    const starfox::render::Framebuffer* fixed_subtract_foreground{};
    std::int32_t fixed_subtract_foreground_x{};
    std::int32_t fixed_subtract_foreground_y{};
    std::uint8_t master_brightness{15U};
    starfox::simulation::PlanetPresentationState planet;
    starfox::simulation::WindowWipeState wipe;
    starfox::simulation::ColourMathEffectState colour_math;
    bool expand_wipe{};
    bool expand_wipe_vertical{};
    bool clip_circle{};
    std::int16_t circle_left{};
    std::int16_t circle_top{};
    std::int16_t circle_right{};
    std::int16_t circle_bottom{};
    const starfox::render::SurfaceBuffer* model_surfaces{};
    std::int32_t model_surface_x{};
    std::int32_t model_surface_y{};
    const std::vector<std::uint8_t>* shadow_mask{};
    std::array<const std::vector<std::uint8_t>*,2> stereo_shadow_masks{};
    starfox::render::shadows::GpuShadowOutput resident_shadow;
    std::array<starfox::render::shadows::GpuShadowOutput,2> stereo_resident_shadows;
    std::uint32_t shadow_width{}, shadow_height{};
    std::int32_t shadow_offset_y{};
    std::uint8_t chromatic_aberration{};
    std::uint8_t hdr_effect{};
    bool touch_controls{};
};

std::uint32_t display_width_for(
    starfox::simulation::DisplayMode mode) noexcept {
    switch (mode) {
    case starfox::simulation::DisplayMode::widescreen_16_9:
        return widescreen_16_9_width;
    case starfox::simulation::DisplayMode::widescreen_16_10:
        return widescreen_16_10_width;
    case starfox::simulation::DisplayMode::ultrawide_21_9:
        return ultrawide_width;
    case starfox::simulation::DisplayMode::super_ultrawide_32_9:
        return super_ultrawide_width;
    case starfox::simulation::DisplayMode::standard_4_3:
    default:
        return snes_width;
    }
}

std::size_t hud_profile_index(
    starfox::simulation::DisplayMode mode,
    starfox::simulation::Experience experience) noexcept {
    auto result = std::size_t{};
    switch (mode) {
    case starfox::simulation::DisplayMode::widescreen_16_9:
        result = 1U;
        break;
    case starfox::simulation::DisplayMode::widescreen_16_10:
        result = 2U;
        break;
    case starfox::simulation::DisplayMode::ultrawide_21_9:
        result = 3U;
        break;
    case starfox::simulation::DisplayMode::super_ultrawide_32_9:
        result = 4U;
        break;
    case starfox::simulation::DisplayMode::standard_4_3:
    default:
        break;
    }
    if (experience == starfox::simulation::Experience::starfox_ex) {
        result += starfox::render::hud_display_profile_count;
    }
    return result;
}

std::string_view display_profile_name(
    starfox::simulation::DisplayMode mode) noexcept {
    switch (mode) {
    case starfox::simulation::DisplayMode::widescreen_16_9:
        return "16 BY 9";
    case starfox::simulation::DisplayMode::widescreen_16_10:
        return "16 BY 10";
    case starfox::simulation::DisplayMode::ultrawide_21_9:
        return "21 BY 9";
    case starfox::simulation::DisplayMode::super_ultrawide_32_9:
        return "32 BY 9";
    case starfox::simulation::DisplayMode::standard_4_3:
    default:
        return "4 BY 3";
    }
}

constexpr std::array<std::string_view,
    starfox::simulation::render_scale_count> render_scale_names{{
    "1X  NATIVE",
    "2X",
    "3X",
    "4X",
}};

ButtonMask with_swapped_face_buttons(
    ButtonMask buttons, bool enabled) noexcept {
    if (!enabled) return buttons;
    const auto a = (buttons & starfox::input::a) != 0U;
    const auto b = (buttons & starfox::input::b) != 0U;
    const auto x = (buttons & starfox::input::x) != 0U;
    const auto y = (buttons & starfox::input::y) != 0U;
    buttons = static_cast<ButtonMask>(buttons
        & ~(starfox::input::a | starfox::input::b
            | starfox::input::x | starfox::input::y));
    if (a) buttons = static_cast<ButtonMask>(buttons | starfox::input::b);
    if (b) buttons = static_cast<ButtonMask>(buttons | starfox::input::a);
    if (x) buttons = static_cast<ButtonMask>(buttons | starfox::input::y);
    if (y) buttons = static_cast<ButtonMask>(buttons | starfox::input::x);
    return buttons;
}

std::uint32_t render_scale_index(
    starfox::simulation::RenderScale scale) noexcept {
    return std::min(static_cast<std::uint32_t>(scale),
        static_cast<std::uint32_t>(
            starfox::simulation::render_scale_count - 1U));
}

std::uint32_t render_scale_factor(
    starfox::simulation::RenderScale scale) noexcept {
    return render_scale_index(scale) + 1U;
}

std::string_view render_scale_name(
    starfox::simulation::RenderScale scale) noexcept {
    return render_scale_names[render_scale_index(scale)];
}

starfox::render::TwoDFilter two_d_filter_backend(
    starfox::simulation::TwoDFilterMode mode) noexcept {
    switch (mode) {
    case starfox::simulation::TwoDFilterMode::edge:
        return starfox::render::TwoDFilter::edge;
    case starfox::simulation::TwoDFilterMode::xbrz:
        return starfox::render::TwoDFilter::xbrz;
    case starfox::simulation::TwoDFilterMode::sharp_bilinear:
        return starfox::render::TwoDFilter::sharp_bilinear;
    case starfox::simulation::TwoDFilterMode::crt:
        return starfox::render::TwoDFilter::crt;
    case starfox::simulation::TwoDFilterMode::scalefx:
        return starfox::render::TwoDFilter::scalefx;
    case starfox::simulation::TwoDFilterMode::off:
    default:
        return starfox::render::TwoDFilter::off;
    }
}

std::string_view two_d_filter_name(
    starfox::simulation::TwoDFilterMode mode) noexcept {
    const auto backend = two_d_filter_backend(mode);
    // Saved optional backends fall back to the built-in filter.
    if (!starfox::render::two_d_filter_compiled_in(backend)) {
        return "EDGE";
    }
    return starfox::render::two_d_filter_name(backend);
}

std::string_view crosshair_colour_name(
    starfox::simulation::CrosshairColour colour) noexcept {
    switch (colour) {
    case starfox::simulation::CrosshairColour::white:
        return "WHITE";
    case starfox::simulation::CrosshairColour::blue:
        return "BLUE";
    case starfox::simulation::CrosshairColour::red:
        return "RED";
    case starfox::simulation::CrosshairColour::yellow:
        return "YELLOW";
    case starfox::simulation::CrosshairColour::cyan:
        return "CYAN";
    case starfox::simulation::CrosshairColour::magenta:
        return "MAGENTA";
    case starfox::simulation::CrosshairColour::orange:
        return "ORANGE";
    case starfox::simulation::CrosshairColour::green:
    default:
        return "GREEN";
    }
}

std::string_view anti_aliasing_name(
    starfox::simulation::AntiAliasingMode mode) noexcept {
    switch (mode) {
    case starfox::simulation::AntiAliasingMode::light:
        return "LIGHT";
    case starfox::simulation::AntiAliasingMode::medium:
        return "MEDIUM";
    case starfox::simulation::AntiAliasingMode::heavy:
        return "HEAVY";
    case starfox::simulation::AntiAliasingMode::off:
    default:
        return "OFF";
    }
}

std::optional<starfox::render::Rgba8> crosshair_tint(
    starfox::simulation::CrosshairColour colour) noexcept {
    switch (colour) {
    case starfox::simulation::CrosshairColour::white:
        return starfox::render::Rgba8{255U, 255U, 255U, 255U};
    case starfox::simulation::CrosshairColour::blue:
        return starfox::render::Rgba8{72U, 136U, 255U, 255U};
    case starfox::simulation::CrosshairColour::red:
        return starfox::render::Rgba8{255U, 64U, 64U, 255U};
    case starfox::simulation::CrosshairColour::yellow:
        return starfox::render::Rgba8{255U, 232U, 64U, 255U};
    case starfox::simulation::CrosshairColour::cyan:
        return starfox::render::Rgba8{64U, 240U, 255U, 255U};
    case starfox::simulation::CrosshairColour::magenta:
        return starfox::render::Rgba8{255U, 96U, 255U, 255U};
    case starfox::simulation::CrosshairColour::orange:
        return starfox::render::Rgba8{255U, 152U, 48U, 255U};
    case starfox::simulation::CrosshairColour::green:
    default:
        return std::nullopt;
    }
}

void apply_crosshair_tint(
    starfox::render::Palette256& palette,
    starfox::simulation::CrosshairColour colour) noexcept {
    const auto tint = crosshair_tint(colour);
    if (!tint) return;
    // SPRITES.ASM reserves OBJ palette 4 for the four tile-061 crosshair
    // quadrants. Tinting this one row cannot affect lives, bombs, portraits,
    // or map sprites. Preserve the tile's source shading while replacing hue.
    constexpr std::size_t first = 128U + 4U * 16U;
    for (std::size_t index = 1U; index < 16U; ++index) {
        const auto source = palette[first + index];
        const auto intensity = std::max({source.r, source.g, source.b});
        palette[first + index] = {
            static_cast<std::uint8_t>(
                static_cast<std::uint32_t>(tint->r) * intensity / 255U),
            static_cast<std::uint8_t>(
                static_cast<std::uint32_t>(tint->g) * intensity / 255U),
            static_cast<std::uint8_t>(
                static_cast<std::uint32_t>(tint->b) * intensity / 255U),
            255U,
        };
    }
    // MHUD's accompanying triangles use this reserved bright entry whenever
    // a non-original colour is selected.
    palette[first + 15U] = *tint;
}

std::uint8_t nearest_palette_index(
    std::span<const starfox::render::Rgba8> palette,
    starfox::render::Rgba8 target) noexcept {
    std::uint8_t best{};
    auto best_distance = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0U;
         index < std::min<std::size_t>(palette.size(), 256U); ++index) {
        const auto colour = palette[index];
        const auto red = static_cast<std::int32_t>(colour.r) - target.r;
        const auto green = static_cast<std::int32_t>(colour.g) - target.g;
        const auto blue = static_cast<std::int32_t>(colour.b) - target.b;
        const auto distance = static_cast<std::uint32_t>(
            red * red + green * green + blue * blue);
        if (distance >= best_distance) continue;
        best = static_cast<std::uint8_t>(index);
        best_distance = distance;
    }
    return best;
}

struct HudRect {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};

    [[nodiscard]] bool contains(float px, float py) const noexcept {
        return px >= static_cast<float>(x)
            && py >= static_cast<float>(y)
            && px < static_cast<float>(x + width)
            && py < static_cast<float>(y + height);
    }
};

HudRect default_hud_rect(
    starfox::render::HudElement element,
    std::uint32_t width,
    starfox::simulation::Experience experience) noexcept {
    switch (element) {
    case starfox::render::HudElement::lives:
        // EX locates lives immediately above its lower-left shield label;
        // retail keeps the same three source sprites at the upper left.
        return experience == starfox::simulation::Experience::starfox_ex
            ? HudRect{26, 174, 26, 10}
            : HudRect{15, 16, 26, 10};
    case starfox::render::HudElement::shield:
        return {20, 179, 48, 25};
    case starfox::render::HudElement::bombs_boost:
        return {static_cast<std::int32_t>(width) - 68, 178, 48, 26};
    case starfox::render::HudElement::comms:
        return {static_cast<std::int32_t>(width) / 2 - 68, 164, 136, 48};
    case starfox::render::HudElement::boss_health:
        // Reserve ENEMY plus the complete legal 8-bit meter span. Meter
        // layers are composited into the 224-line screen at y=16.
        return {static_cast<std::int32_t>(width) - 195, 17, 181, 10};
    case starfox::render::HudElement::count:
    default:
        return {};
    }
}

HudRect placed_hud_rect(
    starfox::render::HudElement element,
    std::uint32_t width,
    const starfox::render::HudLayout& layout,
    starfox::simulation::Experience experience) noexcept {
    auto result = default_hud_rect(element, width, experience);
    const auto offset = layout[element];
    result.x += offset.x;
    result.y += offset.y;
    return result;
}

void clamp_hud_element(
    starfox::render::HudLayout& layout,
    starfox::render::HudElement element,
    std::uint32_t width,
    starfox::simulation::Experience experience) noexcept {
    const auto base = default_hud_rect(element, width, experience);
    auto& offset = layout[element];
    offset.x = static_cast<std::int16_t>(std::clamp<std::int32_t>(offset.x,
        -base.x,
        static_cast<std::int32_t>(width) - base.x - base.width));
    offset.y = static_cast<std::int16_t>(std::clamp<std::int32_t>(offset.y,
        -base.y, static_cast<std::int32_t>(snes_height) - base.y - base.height));
}

void clamp_hud_layout(
    starfox::render::HudLayout& layout,
    std::uint32_t width,
    starfox::simulation::Experience experience) noexcept {
    for (std::uint8_t value = 0U;
         value < static_cast<std::uint8_t>(starfox::render::HudElement::count);
         ++value) {
        clamp_hud_element(layout,
            static_cast<starfox::render::HudElement>(value), width,
            experience);
    }
}

HudRect hud_reset_button_rect(std::uint32_t width) noexcept {
    return {static_cast<std::int32_t>(width) / 2 - 112, 210, 108, 14};
}

HudRect hud_done_button_rect(std::uint32_t width) noexcept {
    return {static_cast<std::int32_t>(width) / 2 + 4, 210, 108, 14};
}

std::vector<ScriptedPress> parse_scripted_presses(const char* text) {
    std::vector<ScriptedPress> result;
    if (text == nullptr || *text == '\0') return result;
    const std::string script{text};
    std::size_t begin = 0U;
    while (begin < script.size()) {
        const auto end = script.find(',', begin);
        const auto separator = script.find(':', begin);
        if (separator == std::string::npos
            || (end != std::string::npos && separator >= end)) {
            throw std::runtime_error{
                "STARFOX_TEST_PRESSES must use frame:button-mask entries"};
        }
        const auto item_end = end == std::string::npos ? script.size() : end;
        result.push_back({
            static_cast<std::uint64_t>(std::stoull(
                script.substr(begin, separator - begin), nullptr, 0)),
            static_cast<ButtonMask>(std::stoul(
                script.substr(separator + 1U, item_end - separator - 1U),
                nullptr, 0)),
        });
        begin = item_end + 1U;
    }
    return result;
}

bool filename_equal_case_insensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    const auto left_name = left.filename().string();
    const auto right_name = right.filename().string();
    if (left_name.size() != right_name.size()) return false;
    return std::equal(left_name.begin(), left_name.end(), right_name.begin(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
}

std::filesystem::path resolve_companion_case(
    const std::filesystem::path& requested) {
    std::error_code error;
    if (std::filesystem::is_regular_file(requested, error)) return requested;
    error.clear();
    auto directory = requested.parent_path();
    if (directory.empty()) directory = ".";
    for (std::filesystem::directory_iterator entries{directory, error}, end;
         !error && entries != end; entries.increment(error)) {
        if (!entries->is_regular_file(error)) {
            error.clear();
            continue;
        }
        if (filename_equal_case_insensitive(
                entries->path(), requested.filename())) {
            return entries->path();
        }
    }
    return requested;
}

#if defined(STARFOX_HAS_EMBEDDED_ASSETS)
std::span<const std::uint8_t> embedded_resource(int identifier) {
#if defined(_WIN32) && !defined(STARFOX_UWP)
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(
        module, MAKEINTRESOURCEW(identifier), MAKEINTRESOURCEW(10));
    if (resource == nullptr) {
        throw std::runtime_error{"embedded Star Fox asset resource is missing"};
    }
    const auto loaded = LoadResource(module, resource);
    const auto size = SizeofResource(module, resource);
    if (loaded == nullptr || size == 0) {
        throw std::runtime_error{"embedded Star Fox asset resource is invalid"};
    }
    const auto* data = static_cast<const std::uint8_t*>(LockResource(loaded));
    if (data == nullptr) {
        throw std::runtime_error{"embedded Star Fox asset resource is invalid"};
    }
    return std::span<const std::uint8_t>{
        data, static_cast<std::size_t>(size)};
#else
    return starfox::assets::embedded_asset(identifier);
#endif
}

RuntimeAssets make_runtime_assets(
    std::vector<std::uint8_t> rom,
    std::string symbols) {
    auto parsed_symbols = starfox::assets::SymbolMap::parse(symbols);
    return {starfox::assets::RomImage{std::move(rom)},
        std::move(parsed_symbols)};
}

struct RuntimeAssetSet {
    RuntimeAssets original;
    RuntimeAssets starfox_ex;
};

std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path) {
    const auto path_text = path.string();
    auto* stream = SDL_IOFromFile(path_text.c_str(), "rb");
    if (stream == nullptr) {
        throw std::runtime_error{"unable to open file: " + path.string()};
    }
    auto bytes = std::vector<std::uint8_t>{};
    const auto expected_size = SDL_GetIOSize(stream);
    if (expected_size >= 0) {
        bytes.resize(static_cast<std::size_t>(expected_size));
        auto offset = std::size_t{};
        while (offset < bytes.size()) {
            const auto count = SDL_ReadIO(
                stream, bytes.data() + offset, bytes.size() - offset);
            if (count == 0U) break;
            offset += count;
        }
        bytes.resize(offset);
    } else {
        auto chunk = std::array<std::uint8_t, 64U * 1024U>{};
        for (;;) {
            const auto count = SDL_ReadIO(stream, chunk.data(), chunk.size());
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
            if (count != chunk.size()) break;
        }
    }
    const auto status = SDL_GetIOStatus(stream);
    static_cast<void>(SDL_CloseIO(stream));
    if (status == SDL_IO_STATUS_ERROR
        || (expected_size >= 0
            && bytes.size() != static_cast<std::size_t>(expected_size))) {
        throw std::runtime_error{"unable to read file: " + path.string()};
    }
    return bytes;
}

struct RuntimeInputDialogState {
    std::atomic<bool> complete{};
    std::string selection;
    std::string error;
};

void SDLCALL runtime_input_dialog_callback(void* userdata,
    const char* const* files, int) {
    auto& state = *static_cast<RuntimeInputDialogState*>(userdata);
    if (files == nullptr) state.error = SDL_GetError();
    else if (files[0] != nullptr) state.selection = files[0];
    state.complete.store(true, std::memory_order_release);
}

std::filesystem::path choose_runtime_input(
    const std::filesystem::path& companion_path,
    [[maybe_unused]] SDL_Renderer* renderer) {
#if defined(__SWITCH__)
    throw std::runtime_error{
        "Starfox-Assets.BIN was not found. Create it on a PC with "
        "starfox_asset_builder, then copy it beside StarFoxEnhanced.nro"};
#elif defined(SDL_PLATFORM_VITA)
    throw std::runtime_error{
        "Starfox-Assets.BIN was not found. Create it on a PC with "
        "starfox_asset_builder, then copy it to "
        "ux0:data/StarFoxEnhanced/Starfox-Assets.BIN"};
#elif defined(STARFOX_UWP)
    if (renderer == nullptr) {
        throw std::runtime_error{
            "Starfox-Assets.BIN was not found in LocalState"};
    }
    // Xbox has no useful native file picker for sideloaded UWP games. Keep
    // the process alive on a clear provisioning screen so Device Portal can
    // expose LocalState, then pick up a companion uploaded while this screen
    // is open. The old behavior showed a black frame and immediately exited.
    auto running = true;
    while (running) {
        const auto candidate = resolve_companion_case(companion_path);
        if (std::filesystem::is_regular_file(candidate)) {
            static_cast<void>(SDL_SetRenderScale(renderer, 1.0F, 1.0F));
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
            return candidate;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || (event.type == SDL_EVENT_KEY_DOWN
                    && event.key.scancode == SDL_SCANCODE_ESCAPE)) {
                running = false;
            }
        }
        int output_width = 640;
        int output_height = 360;
        static_cast<void>(SDL_GetRenderOutputSize(
            renderer, &output_width, &output_height));
        const auto scale = std::max(2.0F, std::min(4.0F,
            std::min(static_cast<float>(output_width) / 640.0F,
                static_cast<float>(output_height) / 360.0F) * 2.0F));
        static_cast<void>(SDL_SetRenderScale(renderer, scale, scale));
        SDL_SetRenderDrawColor(renderer, 5, 17, 34, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const auto logical_width = static_cast<float>(output_width) / scale;
        const auto draw_centered = [renderer, logical_width](
                                       float y, std::string_view text) {
            const auto width = static_cast<float>(text.size() * 8U);
            const auto x = std::max(8.0F, (logical_width - width) * 0.5F);
            const std::string terminated{text};
            static_cast<void>(SDL_RenderDebugText(
                renderer, x, y, terminated.c_str()));
        };
        draw_centered(42.0F, "STARFOX-ASSETS.BIN NOT FOUND");
        draw_centered(66.0F, "CREATE IT ON A PC WITH STARFOX_ASSET_BUILDER");
        draw_centered(82.0F, "UPLOAD IT TO THIS APP'S LOCALSTATE FOLDER");
        draw_centered(106.0F, "THE GAME WILL CONTINUE WHEN THE FILE APPEARS");
        SDL_RenderPresent(renderer);
        SDL_Delay(250U);
    }
    throw std::runtime_error{"runtime asset provisioning was canceled"};
#else
    RuntimeInputDialogState state;
    constexpr std::array filters{
        SDL_DialogFileFilter{"Star Fox data", "bin;sfc;smc"},
        SDL_DialogFileFilter{"All files", "*"},
    };
    SDL_ShowOpenFileDialog(runtime_input_dialog_callback, &state, nullptr,
        filters.data(), static_cast<int>(filters.size()), nullptr, false);
    while (!state.complete.load(std::memory_order_acquire)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                throw std::runtime_error{"runtime input selection was canceled"};
            }
        }
        SDL_Delay(10U);
    }
    if (!state.error.empty()) {
        throw std::runtime_error{
            "unable to open the runtime input picker: " + state.error};
    }
    if (state.selection.empty()) {
        throw std::runtime_error{"runtime input selection was canceled"};
    }
    return std::filesystem::path{state.selection};
#endif
}

struct RetailVariant {
    std::string_view name;
    std::uint32_t crc32;
    int canonicalization_resource;
};

constexpr auto retail_size = std::size_t{1U << 20U};
constexpr auto retail_v12_crc32 = std::uint32_t{0x8fc4e6d0U};
constexpr std::array retail_variants{
    RetailVariant{"Star Fox (USA) (Rev 2)", retail_v12_crc32, 0},
    RetailVariant{"Star Fox (Japan)", 0x41a60b3fU, 120},
    RetailVariant{"Star Fox (Japan) (Rev 1)", 0xad668a41U, 121},
    RetailVariant{"Star Fox (USA)", 0x0bae0941U, 122},
    RetailVariant{"Star Fox (USA) (Rev 1)", 0xb18676b2U, 123},
    RetailVariant{"Starwing (Europe)", 0x865f1a71U, 124},
    RetailVariant{"Starwing (Europe) (Rev 1)", 0xba64da2bU, 125},
    RetailVariant{"Starwing (Germany)", 0xb48ca238U, 126},
};

std::vector<std::uint8_t> canonicalize_retail_rom(
    const std::filesystem::path& path) {
    auto bytes = read_binary_file(path);
    if (bytes.size() == retail_size + 512U) {
        bytes.erase(bytes.begin(), bytes.begin() + 512U);
    }
    const auto checksum = bytes.size() == retail_size
        ? starfox::assets::crc32(bytes)
        : std::uint32_t{};
    const auto variant = std::find_if(retail_variants.begin(),
        retail_variants.end(), [checksum](const RetailVariant& candidate) {
            return candidate.crc32 == checksum;
        });
    if (variant == retail_variants.end()) {
        throw std::runtime_error{
            "the selected file is not a supported unmodified retail Star "
            "Fox/Starwing ROM: " + path.string()};
    }
    if (variant->canonicalization_resource == 0) return bytes;

    auto canonical = starfox::assets::apply_bps_patch(bytes,
        embedded_resource(variant->canonicalization_resource));
    if (canonical.size() != retail_size
        || starfox::assets::crc32(canonical) != retail_v12_crc32) {
        throw std::runtime_error{
            "the embedded canonicalization data for "
            + std::string{variant->name} + " is invalid"};
    }
    return canonical;
}

std::optional<std::pair<std::filesystem::path, std::vector<std::uint8_t>>>
find_required_retail(const std::filesystem::path& executable_directory) {
    std::vector<std::filesystem::path> candidates;
    if (const auto* override_path = std::getenv("STARFOX_RETAIL_ROM");
        override_path != nullptr && *override_path != '\0') {
        // An explicit override is authoritative: report a bad selection
        // rather than silently finding a different ROM elsewhere.
        const auto path = std::filesystem::path{override_path};
        return std::make_pair(path, canonicalize_retail_rom(path));
    }
    constexpr std::array filenames{
        "Star Fox (USA) (Rev 2).sfc",
        "Star Fox (USA) (Rev 1).sfc",
        "Star Fox (USA).sfc",
        "Star Fox (Japan) (Rev 1).sfc",
        "Star Fox (Japan).sfc",
        "Starwing (Europe) (Rev 1).sfc",
        "Starwing (Europe).sfc",
        "Starwing (Germany).sfc",
        "Star Fox v1.2.sfc",
    };
    auto directories = std::vector<std::filesystem::path>{
        executable_directory};
#if defined(_WIN32) && !defined(STARFOX_UWP)
    directories.insert(directories.begin(), std::filesystem::path{
        R"(C:\NTSC-US Super Nintendo System Roms)"});
#else
    if (const auto* home = std::getenv("HOME");
        home != nullptr && *home != '\0') {
        directories.insert(directories.begin(),
            std::filesystem::path{home}
                / "NTSC-US Super Nintendo System Roms");
    }
#endif
#if !defined(STARFOX_UWP)
    // UserDataPaths::Documents is not available to every Xbox UWP account and
    // can raise a WinRT activation exception. Xbox companions live only in
    // the package's guaranteed LocalState directory.
    if (const auto* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
        documents != nullptr && *documents != '\0') {
        directories.emplace_back(documents);
        directories.emplace_back(
            std::filesystem::path{documents} / "Star Fox Enhanced");
    }
#endif
    for (const auto& directory : directories) {
        for (const auto* filename : filenames) {
            candidates.emplace_back(directory / filename);
        }
    }
    for (const auto& path : candidates) {
        if (!std::filesystem::is_regular_file(path)) continue;
        try {
            return std::make_pair(path, canonicalize_retail_rom(path));
        } catch (const std::runtime_error&) {
            // Automatic discovery may encounter a corrupt or modified dump.
            // Continue looking for another supported retail revision; an
            // explicit STARFOX_RETAIL_ROM selection remains authoritative.
        }
    }
    return std::nullopt;
}

std::uint32_t embedded_asset_manifest() {
    return starfox::assets::runtime_companion_manifest(embedded_resource);
}

std::string embedded_text_resource(int identifier) {
    const auto resource = embedded_resource(identifier);
    return {reinterpret_cast<const char*>(resource.data()), resource.size()};
}

RuntimeAssetSet unpack_runtime_assets(
    starfox::assets::RuntimeBundlePayload payload) {
    return {
        make_runtime_assets(std::move(payload.original_rom),
            std::move(payload.original_symbols)),
        make_runtime_assets(std::move(payload.starfox_ex_rom),
            std::move(payload.starfox_ex_symbols)),
    };
}
#endif

std::filesystem::path writable_runtime_directory(
    const std::filesystem::path& executable_directory) {
#if defined(__SWITCH__)
    // Homebrew folders may be renamed, and an NSP forwarder still launches
    // the NRO from its installed SD path. Store and discover companions beside
    // the file that was actually launched instead of assuming one folder name.
    return executable_directory.empty()
        ? std::filesystem::path{"sdmc:/switch/StarFoxEnhanced"}
        : executable_directory;
#elif defined(SDL_PLATFORM_VITA)
    // The installed application directory under ux0:app is read-only.
    // Keep generated assets, optional music, settings, and SRAM together in
    // a stable user-writable location shared by upgrades of the VPK.
    return std::filesystem::path{"ux0:data/StarFoxEnhanced"};
#elif defined(STARFOX_UWP)
    // SDL's normal preference path adds organization/application subfolders
    // below UWP LocalState. Companion files are documented and provisioned
    // through Xbox Device Portal at the LocalState root, so deliberately use
    // that root here while settings remain in their existing subdirectory.
    if (char* preference_path =
            SDL_GetPrefPath("StarFoxEnhanced", "StarFoxEnhanced");
        preference_path != nullptr) {
        auto result = std::filesystem::path{preference_path};
        SDL_free(preference_path);
        if (result.filename().empty()) result = result.parent_path();
        result = result.parent_path().parent_path();
        if (!result.empty()) return result;
    }
#elif defined(SDL_PLATFORM_IOS) || defined(__ANDROID__)
    if (char* preference_path =
            SDL_GetPrefPath("StarFoxEnhanced", "StarFoxEnhanced");
        preference_path != nullptr) {
        const auto result = std::filesystem::path{preference_path};
        SDL_free(preference_path);
        return result;
    }
#endif
    return executable_directory;
}

std::filesystem::path find_msu1_pack(
    const std::filesystem::path& executable_directory) {
    auto candidates = std::vector<std::filesystem::path>{
        executable_directory
            / std::filesystem::path{starfox::audio::msu1_pack_filename}};
#if defined(__SWITCH__)
    candidates.emplace_back(std::filesystem::path{
        "sdmc:/switch/StarFoxEnhanced"} / starfox::audio::msu1_pack_filename);
    candidates.emplace_back(std::filesystem::path{
        "sdmc:/switch/StarFoxEnhanced-switch"}
        / starfox::audio::msu1_pack_filename);
#endif
#if defined(SDL_PLATFORM_VITA)
    candidates.emplace_back(std::filesystem::path{
        "ux0:data/StarFoxEnhanced"} / starfox::audio::msu1_pack_filename);
#endif
#if defined(STARFOX_UWP)
    // Preserve packs copied to the nested SDL preference directory used by
    // the first UWP package while preferring the documented LocalState root.
    if (char* preference_path =
            SDL_GetPrefPath("StarFoxEnhanced", "StarFoxEnhanced");
        preference_path != nullptr) {
        candidates.emplace_back(std::filesystem::path{preference_path}
            / starfox::audio::msu1_pack_filename);
        SDL_free(preference_path);
    }
#endif
#if defined(__APPLE__) && !defined(SDL_PLATFORM_IOS)
    // A macOS bundle keeps the executable in App.app/Contents/MacOS. Also
    // accept the documented companion beside the .app bundle itself.
    candidates.push_back(executable_directory.parent_path().parent_path()
        .parent_path() / starfox::audio::msu1_pack_filename);
#endif
#if !defined(STARFOX_UWP)
    if (const auto* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
        documents != nullptr && *documents != '\0') {
        candidates.emplace_back(std::filesystem::path{documents}
            / "Star Fox Enhanced" / starfox::audio::msu1_pack_filename);
    }
#endif
    candidates.emplace_back(writable_runtime_directory(executable_directory)
        / starfox::audio::msu1_pack_filename);
    for (const auto& requested : candidates) {
        const auto candidate = resolve_companion_case(requested);
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return candidates.front();
}

void write_asset_companion(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes) {
    std::error_code directory_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(
            path.parent_path(), directory_error);
    }
    if (directory_error) {
        throw std::runtime_error{"unable to create asset directory: "
            + path.parent_path().string() + ": "
            + directory_error.message()};
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream{temporary,
            std::ios::binary | std::ios::trunc};
        if (!stream) {
            throw std::runtime_error{
                "unable to create asset companion: " + path.string()};
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw std::runtime_error{
                "unable to write asset companion: " + path.string()};
        }
    }
#if defined(_WIN32) && !defined(STARFOX_UWP)
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        static_cast<void>(DeleteFileW(temporary.c_str()));
        throw std::runtime_error{
            "unable to install asset companion (Windows error "
            + std::to_string(error) + "): " + path.string()};
    }
#else
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error{
            "unable to install asset companion: " + path.string()
            + ": " + error.message()};
    }
#endif
}

#if defined(STARFOX_HAS_EMBEDDED_ASSETS)
RuntimeAssetSet load_or_compile_runtime_assets(
    const std::filesystem::path& executable_directory,
    [[maybe_unused]] SDL_Renderer* renderer = nullptr) {
    const auto companion_path =
        writable_runtime_directory(executable_directory)
            / "Starfox-Assets.BIN";
    const auto manifest = embedded_asset_manifest();
    auto companion_candidates =
        std::vector<std::filesystem::path>{companion_path};
#if defined(__SWITCH__)
    // Keep the documented path plus the first release archive's outer-folder
    // name as fallbacks for loaders that omit the NRO path from argv[0].
    companion_candidates.emplace_back(
        "sdmc:/switch/StarFoxEnhanced/Starfox-Assets.BIN");
    companion_candidates.emplace_back(
        "sdmc:/switch/StarFoxEnhanced-switch/Starfox-Assets.BIN");
#endif
#if defined(SDL_PLATFORM_VITA)
    companion_candidates.emplace_back(
        "ux0:data/StarFoxEnhanced/Starfox-Assets.BIN");
#endif
#if defined(STARFOX_UWP)
    // Migrate companions provisioned according to the first UWP build's
    // nested SDL preference path into the now-documented LocalState root.
    if (char* preference_path =
            SDL_GetPrefPath("StarFoxEnhanced", "StarFoxEnhanced");
        preference_path != nullptr) {
        companion_candidates.emplace_back(
            std::filesystem::path{preference_path} / "Starfox-Assets.BIN");
        SDL_free(preference_path);
    }
#endif
#if !defined(STARFOX_UWP)
    if (const auto* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
        documents != nullptr && *documents != '\0') {
        companion_candidates.emplace_back(
            std::filesystem::path{documents} / "Starfox-Assets.BIN");
        companion_candidates.emplace_back(std::filesystem::path{documents}
            / "Star Fox Enhanced" / "Starfox-Assets.BIN");
    }
#endif
    for (const auto& requested : companion_candidates) {
        const auto candidate = resolve_companion_case(requested);
        if (!std::filesystem::is_regular_file(candidate)) continue;
        try {
            const auto bytes = read_binary_file(candidate);
            auto assets = unpack_runtime_assets(
                starfox::assets::decode_runtime_bundle(bytes, manifest));
            if (candidate != companion_path) {
                write_asset_companion(companion_path, bytes);
            }
            return assets;
        } catch (const std::exception&) {
            // An update can legitimately invalidate a previously compiled
            // companion. Rebuild it below from the user's validated retail
            // image; if that image is unavailable, the resulting error tells
            // the user exactly how to supply it.
        }
    }

    auto retail = find_required_retail(executable_directory);
    if (!retail) {
        const auto selected = choose_runtime_input(companion_path, renderer);
        const auto selected_bytes = read_binary_file(selected);
        constexpr std::array<std::uint8_t, 8> bundle_magic{
            'S', 'F', 'O', 'X', 'A', 'S', '0', '1'};
        if (selected_bytes.size() >= bundle_magic.size()
            && std::equal(bundle_magic.begin(), bundle_magic.end(),
                selected_bytes.begin())) {
            auto assets = unpack_runtime_assets(
                starfox::assets::decode_runtime_bundle(
                    selected_bytes, manifest));
            write_asset_companion(companion_path, selected_bytes);
            return assets;
        }
        retail.emplace(selected, canonicalize_retail_rom(selected));
    }
    auto [retail_path, retail_rom] = std::move(*retail);
    static_cast<void>(retail_path);
    starfox::assets::RuntimeBundlePayload payload;
    payload.original_rom = starfox::assets::apply_bps_patch(
        retail_rom, embedded_resource(101));
    payload.original_symbols = embedded_text_resource(102);
    payload.starfox_ex_rom = starfox::assets::apply_bps_patch(
        retail_rom, embedded_resource(108));
    payload.starfox_ex_symbols = embedded_text_resource(109);
    const auto companion =
        starfox::assets::encode_runtime_bundle(payload, manifest);
    write_asset_companion(companion_path, companion);
    return unpack_runtime_assets(std::move(payload));
}
#endif

RuntimeAssets load_external_assets(
    const std::filesystem::path& rom_path,
    const std::filesystem::path& symbols_path) {
    return {
        starfox::assets::RomImage::load(rom_path),
        starfox::assets::SymbolMap::load(symbols_path),
    };
}

class SdlContext {
public:
    SdlContext() {
        starfox::app::configure_native_gamepad_support();
#if defined(__SWITCH__)
        // AUDOUT owns two device buffers.  Keep each one short enough that
        // button/fire audio remains perceptually attached to its video frame.
        static_cast<void>(SDL_SetHint(
            SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512"));
#endif
#if defined(SDL_PLATFORM_VITA)
        // SDL exposes both Vita touch surfaces as mice by default.  This
        // native-controller target does not draw mobile controls, and the
        // rear pad in particular must never steer or click game UI.
        static_cast<void>(SDL_SetHint(
            SDL_HINT_TOUCH_MOUSE_EVENTS, "0"));
        static_cast<void>(SDL_SetHint(
            SDL_HINT_VITA_ENABLE_BACK_TOUCH, "0"));
#endif
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
            throw std::runtime_error{std::string{"SDL_Init: "} + SDL_GetError()};
        }
    }

    ~SdlContext() { SDL_Quit(); }
    SdlContext(const SdlContext&) = delete;
    SdlContext& operator=(const SdlContext&) = delete;
};

class Window {
public:
    explicit Window(starfox::simulation::RendererMode renderer_mode,DlssHost* dlss=nullptr):dlss_(dlss) {
#if defined(STARFOX_UWP) || defined(__ANDROID__)
        constexpr auto window_flags = SDL_WINDOW_FULLSCREEN;
#else
        constexpr auto window_flags = SDL_WINDOW_RESIZABLE;
#endif
        window_ = SDL_CreateWindow(
            "Star Fox Enhanced - native PC runtime", 1024, 896,
            window_flags | (std::getenv("STARFOX_TEST_HIDDEN") ? SDL_WINDOW_HIDDEN : 0));
        if (window_ == nullptr) {
            throw std::runtime_error{
                std::string{"SDL_CreateWindow: "} + SDL_GetError()};
        }
        recreate_renderer(renderer_mode);
        if(!std::getenv("STARFOX_TEST_HIDDEN")) SDL_ShowWindow(window_);
        static_cast<void>(SDL_SyncWindow(window_));
        // Put an actual black frame on the desktop before ROM decoding, game
        // construction or audio-device setup can begin. A merely-created SDL
        // window can remain compositor-transparent until its first present.
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderPresent(renderer_);
        static_cast<void>(SDL_SyncWindow(window_));
    }

    ~Window() {
        gpu_effects_.release_device(); // Release COM objects before SDL unloads the graphics driver.
        sdl_gpu_effects_.release_device();
        native_composite_.release_device();
        temporal_composite_.release_device();
        late_scene_.release_device();
        background_scene_.release_device();
        native_raster_.release_device();
        native_scene_.release_device();native_stereo_scene_.release_device();release_stereo_textures();recorded_scene_=nullptr;
        resident_shadows_.release_device();
        for(auto& shadows:stereo_resident_shadows_) shadows.release_device();
        native_dxr_shadows_.release_device();
        for(auto& shadows:stereo_native_dxr_shadows_) shadows.release_device();
        SDL_DestroyTexture(bloom_texture_);
        SDL_DestroyTexture(smooth_model_texture_);
        SDL_DestroyTexture(smooth_target_texture_);
        SDL_DestroyTexture(texture_);
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] SDL_Renderer* renderer() const noexcept { return renderer_; }

    [[nodiscard]] bool native_gpu_enabled() const noexcept {
#if defined(STARFOX_SDL_GPU_EFFECTS)
        auto* device = static_cast<SDL_GPUDevice*>(SDL_GetPointerProperty(
            SDL_GetRendererProperties(renderer_), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
        return portable_gpu_ && device
            && (SDL_GetGPUShaderFormats(device) & (SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL))
            && !std::getenv("STARFOX_DISABLE_GPU_EFFECTS")
            && (!std::getenv("STARFOX_DISABLE_GPU_NATIVE")
                || std::getenv("STARFOX_TEST_GPU_NATIVE_PIPELINE"));
#else
        return false;
#endif
    }

    void reset_temporal_history() {
        temporal_history_.reset();temporal_draws_.clear();temporal_pending_=false;++temporal_epoch_;
    }
    void begin_temporal_frame(std::uint64_t scene,std::uint32_t context) {
        // Opt-in until full world inputs and the DLSS evaluator are connected.
        const bool enabled=std::getenv("STARFOX_TEST_TEMPORAL_INPUTS")!=nullptr;
        if(enabled!=temporal_enabled_) reset_temporal_history();
        temporal_enabled_=enabled;
        if(!temporal_enabled_) return;
        last_present_succeeded_=false;
        ++temporal_serial_;
        if(scene!=temporal_scene_ || context!=temporal_context_) reset_temporal_history();
        temporal_scene_=scene;temporal_context_=context;
        temporal_pending_=false;temporal_draws_.clear();
    }
    void finish_temporal_frame(bool presented) {
        if(!temporal_enabled_) return;
        if(presented && last_present_succeeded_ && temporal_pending_) {
            temporal_history_.commit(temporal_draws_,temporal_frame_);
            if(std::getenv("STARFOX_TRACE_GPU")) {
                const auto previous=std::count_if(temporal_draws_.begin(),temporal_draws_.end(),[](const auto& item){
                    const auto* draw=std::get_if<starfox::render::GpuModelDraw>(&item);
                    return draw && draw->previous_pose.has_value();});
                std::cerr<<"temporal-inputs: serial="<<temporal_serial_<<" previous="<<previous
                    <<" depth="<<bool(native_output().geometry_depth)<<" motion="<<bool(native_output().motion)<<'\n';
            }
        } else temporal_history_.reset();
        temporal_pending_=false;temporal_draws_.clear();
    }
    bool submit_native(starfox::render::RasterCommands& commands,bool surfaces) {
        recorded_scene_=nullptr;
        stereo_scene_ready_=false;
        if(!portable_gpu_ || !effect_device() || std::getenv("STARFOX_DISABLE_GPU_EFFECTS")) return false;
        SDL_FlushRenderer(renderer_);
        return native_raster_.render_resident(effect_device(),commands,surfaces,native_gpu_binning_);
    }
    bool submit_scene(const starfox::render::GpuSceneRecording& recording,unsigned width,unsigned height,unsigned stereo_mode=0) {
        recorded_scene_=nullptr;
        stereo_scene_ready_=false;
        if(!native_gpu_enabled()) return false;
        SDL_FlushRenderer(renderer_);
        if(stereo_mode!=0 && submit_stereo_scene(recording,width,height,6.4,512)) {
            recorded_scene_=&recording;stereo_scene_ready_=true;return true;
        }
        recorded_scene_=&recording;
        std::span<const starfox::render::GpuSceneDraw> draws=recording.draws();
        if(temporal_enabled_ && stereo_mode==0) {
            temporal_frame_={temporal_serial_,temporal_epoch_,width,height};
            temporal_draws_=temporal_history_.prepare(draws,temporal_frame_);draws=temporal_draws_;
        }
        if(!native_scene_.render_resident(effect_device(),width,height,draws)) {
            temporal_pending_=false;
            if(!scene_failure_reported_) {
                std::cerr<<"native-geometry: "<<native_scene_.status()<<"; replaying complete frame\n";
                scene_failure_reported_=true;
            }
            return false;
        }
        recorded_scene_=&recording;
        temporal_pending_=temporal_enabled_ && stereo_mode==0;
        if(!scene_success_reported_ && std::any_of(recording.draws().begin(),recording.draws().end(),
            [](const auto& draw){return std::holds_alternative<starfox::render::GpuModelDraw>(draw);})) {
            std::cerr<<"native-geometry: GPU model batch resident\n";scene_success_reported_=true;
        }
        return true;
    }
    starfox::render::GpuRasterOutput native_output() const {
        return recorded_scene_?native_scene_.resident_output():native_raster_.resident_output();
    }
    bool submit_stereo_scene(const starfox::render::GpuSceneRecording& recording,
        unsigned width,unsigned height,double separation,double convergence) {
        if(!native_gpu_enabled()) return false;
        SDL_FlushRenderer(renderer_);
        return native_stereo_scene_.render_resident(effect_device(),width,height,
            recording.draws(),separation,convergence);
    }
    void release_stereo_textures() noexcept {
        stereo_scene_ready_=false;
        for(auto*& eye:stereo_eye_textures_) {SDL_DestroyTexture(eye);eye=nullptr;}
        SDL_DestroyTexture(stereo_packed_texture_);stereo_packed_texture_=nullptr;
        stereo_packed_width_=0;
        stereo_eye_width_=stereo_eye_height_=0;
    }
    bool retain_stereo_eye(unsigned eye) {
        if(eye>1 || !texture_ || !native_gpu_enabled()) return false;
        // Preserve the model layer's high-resolution composition when enabled.
        unsigned width=texture_width_,height=texture_height_;
        if(smooth_layer_ready_) {
            ensure_1440p_model_textures(width,height);
            float w{},h{};
            if(!SDL_GetTextureSize(smooth_target_texture_,&w,&h)) return false;
            width=static_cast<unsigned>(w);height=static_cast<unsigned>(h);
        }
        if(width!=stereo_eye_width_ || height!=stereo_eye_height_) {
            release_stereo_textures();
            stereo_eye_width_=width;stereo_eye_height_=height;
        }
        if(!stereo_eye_textures_[eye]) {
            stereo_eye_textures_[eye]=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_TARGET,static_cast<int>(width),static_cast<int>(height));
            if(!stereo_eye_textures_[eye]) return false;
        }
        auto* previous=SDL_GetRenderTarget(renderer_);
        if(!SDL_SetRenderTarget(renderer_,stereo_eye_textures_[eye])) return false;
        const bool ready=SDL_SetRenderDrawColor(renderer_,0,0,0,255)
            && SDL_RenderClear(renderer_)
            && SDL_RenderTexture(renderer_,texture_,nullptr,nullptr)
            && (!smooth_layer_ready_ || SDL_RenderTexture(renderer_,smooth_model_texture_,nullptr,nullptr))
            && (!bloom_layer_ready_ || SDL_RenderTexture(renderer_,bloom_texture_,nullptr,nullptr));
        const bool restored=SDL_SetRenderTarget(renderer_,previous);
        // Finish recording these reads before the next eye reuses effect textures.
        return ready && restored && SDL_FlushRenderer(renderer_);
    }
    bool present_stereo(const starfox::render::GpuSceneRecording& recording,
        const starfox::render::Framebuffer& frame,std::span<const starfox::render::Rgba8> palette,
        const starfox::simulation::CircleEffectState& circle,const PresentationEffects& effects,
        const starfox::render::RasterCommands& commands,unsigned scale,
        const starfox::render::LayerCompositeSettings& layer,unsigned mode) {
        const auto failed=[](const char* stage) {
            if(std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"stereo failure: "<<stage<<": "<<SDL_GetError()<<'\n';
            return false;
        };
        if(std::getenv("STARFOX_TEST_FAIL_STEREO_PRESENT")) return failed("injected presentation failure");
        if(mode<1 || mode>2 || (!stereo_scene_ready_
            && !submit_stereo_scene(recording,commands.width(),commands.height(),6.4,512))) return failed("scene submission");
        for(unsigned eye=0;eye<2;++eye) {
            const auto output=native_stereo_scene_.resident_output(eye);
            auto eye_effects=effects;
            eye_effects.late_dust_eye_x=eye?3.2F:-3.2F;
            if(effects.shadow_mask || effects.resident_shadow.buffer
                || effects.stereo_shadow_masks[0] || effects.stereo_shadow_masks[1]
                || effects.stereo_resident_shadows[0].buffer || effects.stereo_resident_shadows[1].buffer) {
                if(!effects.stereo_shadow_masks[eye] && !effects.stereo_resident_shadows[eye].buffer)
                    return failed("missing eye shadow mask");
                eye_effects.shadow_mask=effects.stereo_shadow_masks[eye];
                eye_effects.resident_shadow=effects.stereo_resident_shadows[eye];
            }
            if(!present_native(frame,palette,circle,eye_effects,commands,scale,layer,&output,false)) return failed("eye effects");
            if(!retain_stereo_eye(eye)) return failed("eye retention");
        }
        const unsigned packed_width=stereo_eye_width_*(mode==2?2U:1U);
        if(!stereo_packed_texture_ || stereo_packed_width_!=packed_width) {
            SDL_DestroyTexture(stereo_packed_texture_);
            stereo_packed_texture_=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_TARGET,int(packed_width),int(stereo_eye_height_));
            stereo_packed_width_=packed_width;
            if(!stereo_packed_texture_) return false;
        }
        if(!SDL_SetRenderTarget(renderer_,stereo_packed_texture_)) return false;
        bool ready=SDL_SetRenderDrawColor(renderer_,0,0,0,255) && SDL_RenderClear(renderer_);
        for(unsigned eye=0;eye<2 && ready;++eye) {
            const float half=float(packed_width)*.5F;
            const SDL_FRect destination{eye*half,0,half,float(stereo_eye_height_)};
            ready=SDL_SetTextureScaleMode(stereo_eye_textures_[eye],mode==1?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST)
                && SDL_RenderTexture(renderer_,stereo_eye_textures_[eye],nullptr,&destination);
        }
        const bool restored=SDL_SetRenderTarget(renderer_,nullptr);
        if(!ready || !restored) return false;
        if(const auto* path=std::getenv("STARFOX_CAPTURE_PRESENTATION_PATH")) {
            if(const auto* frames=std::getenv("STARFOX_TEST_FRAMES")) {
                const bool final_capture=++stereo_capture_frames_==std::stoull(frames);
                const bool capture_sequence=std::getenv("STARFOX_CAPTURE_PRESENTATION_SEQUENCE")!=nullptr;
                if(final_capture || capture_sequence) {
                    if(!SDL_SetRenderTarget(renderer_,stereo_packed_texture_)) return false;
                    auto* capture=SDL_RenderReadPixels(renderer_,nullptr);
                    SDL_SetRenderTarget(renderer_,nullptr);
                    if(!capture) return false;
                    bool saved=true;
                    if(capture_sequence) {
                        const auto frame_path=std::string(path)+".frame-"+std::to_string(stereo_capture_frames_)+".bmp";
                        saved=SDL_SaveBMP(capture,frame_path.c_str());
                    }
                    if(final_capture) saved=SDL_SaveBMP(capture,path) && saved;
                    SDL_DestroySurface(capture);
                    if(!saved) return false;
                }
            }
        }
        // Full SBS has twice the display aspect; Half SBS retains the mono
        // display aspect and intentionally squeezes each eye horizontally.
        const auto display_width=starfox::render::presentation_width(texture_width_,texture_height_)
            *(mode==2?2U:1U);
        if(!SDL_SetRenderLogicalPresentation(renderer_,int(display_width),int(texture_height_),
            SDL_LOGICAL_PRESENTATION_LETTERBOX)) return false;
        stereo_display_active_=true;
        SDL_SetRenderDrawColor(renderer_,0,0,0,255);SDL_RenderClear(renderer_);
        if(!SDL_RenderTexture(renderer_,stereo_packed_texture_,nullptr,nullptr)) return false;
        SDL_RenderPresent(renderer_);
        if(std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"stereo presented: "<<packed_width<<'x'<<stereo_eye_height_<<'\n';
        return true;
    }

    bool submit_shadows(const starfox::render::shadows::Scene& scene,
        starfox::render::shadows::Camera camera,starfox::render::shadows::Vec3 light,
        std::optional<starfox::render::shadows::ReceiverPlane> ground,bool hardware=false,bool geometry_ready=false) {
        native_shadow_selected_=hardware;
        if(hardware) {
            if(!portable_gpu_ || std::getenv("STARFOX_DISABLE_GPU_EFFECTS")) return false;
        } else if(!native_gpu_enabled()) return false;
        SDL_FlushRenderer(renderer_);
        if(hardware) {
            const auto geometry=native_scene_.ray_geometry_output();
            if(geometry_ready) {
                const bool success=!stereo_scene_ready_ && geometry.complete && geometry.vertex_count
                    && native_dxr_shadows_.render_resident(effect_device(),scene,camera,light,ground,&geometry);
                if(!success && std::getenv("STARFOX_TRACE_GPU_RAYS")) std::cerr<<"ray-scene declined: complete="<<geometry.complete
                    <<" vertices="<<geometry.vertex_count<<" status="<<native_dxr_shadows_.status()<<'\n';
                return success;
            }
            return native_dxr_shadows_.render_resident(effect_device(),scene,camera,light,ground);
        }
        return resident_shadows_.render_resident(effect_device(),scene,camera,light,ground);
    }
    auto shadow_output() const { return native_shadow_selected_?native_dxr_shadows_.output():resident_shadows_.output(); }
    bool shadow_gpu_geometry() const { return native_shadow_selected_
        && native_dxr_shadows_.status()=="GPU-resident SDL geometry and DXR shadows"; }
    bool submit_stereo_shadows(unsigned eye,const starfox::render::shadows::Scene& scene,
        starfox::render::shadows::Camera camera,starfox::render::shadows::Vec3 light,
        std::optional<starfox::render::shadows::ReceiverPlane> ground,bool hardware=false,bool geometry_ready=false) {
        if(eye>=2) return false;
        if(hardware) {
            if(!portable_gpu_ || std::getenv("STARFOX_DISABLE_GPU_EFFECTS")) return false;
        } else if(!native_gpu_enabled()) return false;
        stereo_native_shadow_selected_[eye]=hardware;
        SDL_FlushRenderer(renderer_);
        if(hardware) {
            const auto geometry=native_stereo_scene_.ray_geometry_output(eye);
            if(geometry_ready) return stereo_scene_ready_ && geometry.complete && geometry.vertex_count
                && stereo_native_dxr_shadows_[eye].render_resident(effect_device(),scene,camera,light,ground,&geometry);
            return stereo_native_dxr_shadows_[eye].render_resident(effect_device(),scene,camera,light,ground);
        }
        return stereo_resident_shadows_[eye].render_resident(effect_device(),scene,camera,light,ground);
    }
    auto stereo_shadow_output(unsigned eye) const { return stereo_native_shadow_selected_.at(eye)
        ?stereo_native_dxr_shadows_.at(eye).output():stereo_resident_shadows_.at(eye).output(); }
    bool stereo_shadow_gpu_geometry(unsigned eye) const { return stereo_native_shadow_selected_.at(eye)
        && stereo_native_dxr_shadows_.at(eye).status()=="GPU-resident SDL geometry and DXR shadows"; }

    bool present_native(const starfox::render::Framebuffer& frame,
        std::span<const starfox::render::Rgba8> palette,
        const starfox::simulation::CircleEffectState& circle,const PresentationEffects& effects,
        const starfox::render::RasterCommands& commands,unsigned source_scale,
        const starfox::render::LayerCompositeSettings& layer,
        const starfox::render::GpuRasterOutput* eye_output=nullptr,
        bool display_frame=true,bool force_replay=false) {
        const auto source=eye_output?*eye_output:native_output();
        const bool visible_circle=circle.active && circle.radius!=0U
            && (circle.affected_layers&0x3fU)!=0U;
        std::optional<starfox::render::GpuEffectSettings::Circle> gpu_circle;
        if(visible_circle) {
            const auto scale=int(frame.draw_scale());
            starfox::render::GpuEffectSettings::Circle c;
            c.x=int(circle.centre_x)*scale;c.y=int(circle.centre_y)*scale;
            c.radius=int(circle.radius)*scale;
            c.left=effects.clip_circle?effects.circle_left*scale:0;
            c.top=effects.clip_circle?effects.circle_top*scale:0;
            c.right=effects.clip_circle?effects.circle_right*scale:int(frame.stored_width());
            c.bottom=effects.clip_circle?effects.circle_bottom*scale:int(frame.stored_height());
            const auto fixed=[&](unsigned value) {
                value&=31U;
                return ((((value<<3U)|(value>>2U))*std::min<unsigned>(effects.master_brightness,15U))/15U)>>3U;
            };
            c.red=fixed(circle.red);c.green=fixed(circle.green);c.blue=fixed(circle.blue);
            c.subtract=(circle.affected_layers&0x80U)!=0U;
            c.half=(circle.affected_layers&0x40U)!=0U;
            c.affect_sprites=(circle.affected_layers&0x10U)!=0U;
            // Integer disk tests stay exact without requiring shader int64.
            if(c.radius<=16383 && std::abs(c.x)+frame.stored_width()<=16383U
                && std::abs(c.y)+frame.stored_height()<=16383U) gpu_circle=c;
        }
        const bool compatible_background_mask=!effects.fixed_subtract_foreground
            || (effects.fixed_subtract_foreground_x==layer.offset_x
                && effects.fixed_subtract_foreground_y==layer.offset_y);
        std::optional<starfox::render::GpuEffectSettings::HostOverlay> host_overlay;
        if(effects.host_overlay && std::uint64_t(effects.host_overlay->width())*effects.host_overlay->height()<=6144) {
            starfox::render::GpuEffectSettings::HostOverlay h;
            h.x=effects.host_overlay_x;h.y=effects.host_overlay_y;
            h.width=effects.host_overlay->width();h.height=effects.host_overlay->height();
            for(unsigned y=0;y<h.height;++y) for(unsigned x=0;x<h.width;++x) {
                const auto i=y*h.width+x;
                if(effects.host_overlay->get(x,y)!=0) h.bits[i/32]|=1U<<(i%32);
            }
            host_overlay=h;
        }
        std::optional<starfox::render::GpuEffectSettings::HostOverlay> confirmation_overlay;
        if(effects.confirmation_overlay && std::uint64_t(effects.confirmation_overlay->width())*effects.confirmation_overlay->height()<=6144) {
            starfox::render::GpuEffectSettings::HostOverlay h;
            h.width=effects.confirmation_overlay->width();h.height=effects.confirmation_overlay->height();
            h.x=(int(frame.width())-int(h.width))/2;h.y=(int(frame.height())-int(h.height))/2;
            for(unsigned y=0;y<h.height;++y) for(unsigned x=0;x<h.width;++x) {
                const auto i=y*h.width+x;
                if(effects.confirmation_overlay->get(x,y)!=0) h.bits[i/32]|=1U<<(i%32);
            }
            confirmation_overlay=h;
        }
        const bool steady=(!visible_circle || gpu_circle)
            && (!effects.host_overlay || host_overlay) && (!effects.confirmation_overlay || confirmation_overlay)
            && compatible_background_mask;
        if(effects.wipe.active && !steady && std::getenv("STARFOX_TRACE_GPU") && !native_wipe_fallback_reported_) {
            std::cerr<<"native-wipe fallback: horizontal="<<effects.wipe.horizontal_opening
                <<" circle="<<circle.active<<" radius="<<circle.radius
                <<" math="<<effects.colour_math.active<<" overlay="<<bool(effects.overlay)
                <<" text="<<bool(effects.text_overlay)<<" host="<<bool(effects.host_overlay)
                <<" smooth="<<smooth_polys_<<'\n';
            native_wipe_fallback_reported_=true;
        }
        starfox::render::GpuRasterOutput late_output;
        const bool has_late=effects.late_dust || effects.late_cartridge;
        bool late_ready=!has_late;
        if(!force_replay && steady && has_late
            && !std::getenv("STARFOX_TEST_FAIL_LATE_GPU")) {
            std::vector<starfox::render::GpuSceneDraw> draws;
            if(effects.late_cartridge) draws.assign(effects.late_cartridge->scene.draws().begin(),effects.late_cartridge->scene.draws().end());
            if(effects.late_dust) draws.emplace_back(starfox::render::GpuDustDraw{*effects.late_dust,frame.draw_scale(),effects.late_dust_eye_x,512});
            late_ready=late_scene_.render_resident(effect_device(),frame.stored_width(),frame.stored_height(),draws);
            if(late_ready) late_output=late_scene_.resident_output();
        }
        starfox::render::GpuCompositeBackground background_output;
        bool background_ready=!effects.background;
        if(!force_replay && steady && effects.background && !std::getenv("STARFOX_TEST_FAIL_BACKGROUND_GPU")) {
            background_ready=background_scene_.render_resident(effect_device(),frame.stored_width(),frame.stored_height(),effects.background->scene.draws());
            if(background_ready) background_output={background_scene_.resident_output(),effects.background_cpu_coverage,effects.background->margin_origin,256,effects.background->repair_margins};
        }
        if(!force_replay && steady && late_ready && background_ready && native_composite_.compose(source,source_scale,
                frame,frame.write_coverage(),layer,palette,has_late?&late_output:nullptr,
                effects.background?&background_output:nullptr)) {
            if(effects.background && std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"native-background: GPU resident ordered layers\n";
            if(effects.background && effects.background->repair_margins && std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"native-background: GPU EX logo repair\n";
            if(effects.late_cartridge && std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"native-pipeline: GPU late title layers\n";
            window_scale_=frame.draw_scale();gpu_frame_pending_=false;smooth_layer_ready_=false;
            ensure_dimensions(frame.stored_width(),frame.stored_height());
            starfox::render::GpuEffectSettings settings;
            settings.circle=gpu_circle;
            settings.overlay_palette=palette;
            if(effects.overlay) settings.subtractive_overlays[0]=starfox::render::GpuEffectSettings::SubtractiveOverlay{
                effects.overlay,effects.overlay_brightness};
            if(effects.text_overlay) settings.subtractive_overlays[1]=starfox::render::GpuEffectSettings::SubtractiveOverlay{
                effects.text_overlay,effects.text_overlay_brightness};
            if(effects.planet.isolate_fade || effects.planet.level_fade) {
                const auto& f=effects.planet;
                settings.planet_fade=starfox::render::GpuEffectSettings::PlanetFade{
                    f.isolate_left,f.isolate_top,f.isolate_right,f.isolate_bottom,
                    f.isolate_amount,f.level_fade_amount,f.isolate_fade,f.level_fade};
            }
            settings.host_overlay=host_overlay;
            settings.confirmation_overlay=confirmation_overlay;
            settings.touch_controls=effects.touch_controls;
            if(effects.setup_overlay) settings.setup_overlay=starfox::render::GpuEffectSettings::SetupOverlay{
                effects.setup_overlay,effects.setup_left,effects.setup_right,effects.setup_brightness};
            settings.background_subtract=effects.background_fixed_white_subtract;
            settings.background_subtract_protect_models=effects.fixed_subtract_foreground!=nullptr;
            if(effects.colour_math.active && effects.colour_math.affected_layers!=0U) {
                const auto expand=[](unsigned v) {v&=31U;return std::uint8_t((v<<3U)|(v>>2U));};
                const auto& c=effects.colour_math;
                settings.colour_math=starfox::render::GpuEffectSettings::ColourMath{
                    expand(c.red),expand(c.green),expand(c.blue),c.subtract,c.half,
                    (c.affected_layers&0x10U)!=0U};
            }
            if(effects.wipe.active && !effects.wipe.horizontal_opening) {
                starfox::render::GpuEffectSettings::WindowMask w;
                for(std::size_t row=0;row<w.rows.size();++row)
                    w.rows[row]=(effects.wipe.left[row]&255U)|((effects.wipe.right[row]&255U)<<8U);
                w.origin_x=int(frame.width()>snes_width?(frame.width()-snes_width)/2:0);
                w.origin_y=superfx_offset_y;w.logic=effects.wipe.logic;
                w.expand_x=effects.expand_wipe;w.expand_y=effects.expand_wipe_vertical;
                settings.window_mask=w;
            } else if(effects.wipe.active) {
                starfox::render::GpuEffectSettings::HorizontalWipe w;
                w.band_top=w.open_top=static_cast<int>(frame.stored_height());
                w.band_bottom=w.open_bottom=0;
                for(unsigned y=0;y<frame.stored_height();++y) {
                    const double logical_y=(double(y)+.5)/frame.draw_scale();
                    const double source_y=effects.expand_wipe_vertical
                        ?logical_y*192.0/frame.height():logical_y-superfx_offset_y;
                    if(source_y<0 || source_y>=192) continue;
                    w.band_top=std::min(w.band_top,int(y));w.band_bottom=int(y)+1;
                    if(source_y>=effects.wipe.opening_top && source_y<effects.wipe.opening_bottom) {
                        w.open_top=std::min(w.open_top,int(y));w.open_bottom=int(y)+1;
                    }
                }
                w.expanded=effects.expand_wipe;
                w.guard_width=std::max(1U,(frame.width()+221U)/223U)*frame.draw_scale();
                w.origin_x=int((frame.width()-snes_width)/2);
                settings.horizontal_wipe=w;
            }
            settings.filter=static_cast<unsigned>(two_d_filter_);
            const auto* highlight=std::getenv("STARFOX_2D_FILTER_DEBUG");
            settings.highlight_filter=highlight && std::string_view{highlight}!="0";
            settings.lighting=rtx_lighting_;settings.hdr=effects.hdr_effect;settings.chromatic=effects.chromatic_aberration;
            settings.smoothing=model_smoothing_;settings.model_effect=static_cast<unsigned>(effect_);
            settings.world_effect=static_cast<unsigned>(world_effect_);
            settings.model_intensity=effect_intensity_;settings.world_intensity=world_effect_intensity_;
            settings.bloom_model=bloom_;settings.bloom_world=bloom_2d_;
            settings.anti_aliasing=static_cast<unsigned>(anti_aliasing_);
            if(effects.shadow_mask || effects.resident_shadow.buffer) {
                if(effects.shadow_mask) settings.shadow_mask=*effects.shadow_mask;
                settings.resident_shadow=effects.resident_shadow;settings.shadow_width=effects.shadow_width;
                settings.shadow_height=effects.shadow_height;settings.shadow_offset_y=effects.shadow_offset_y;
                settings.shadow_before_style=true;
            }
            settings.presentation_texture=effect_texture(texture_);
            bloom_layer_ready_=bloom_ || bloom_2d_;
            if(bloom_layer_ready_) {
                ensure_bloom_texture(frame.stored_width(),frame.stored_height());
                settings.presentation_glow_texture=effect_texture(bloom_texture_);
            }
            const bool has_model_layer=smooth_polys_ && source.surfaces
                && (recorded_scene_ || std::any_of(commands.commands.begin(),commands.commands.end(),
                    [](const auto& command){return command.has_surface!=0;}));
            if(has_model_layer) {
                ensure_1440p_model_textures(frame.stored_width(),frame.stored_height());
                settings.presentation_model_texture=effect_texture(smooth_model_texture_);
            }
            SDL_FlushRenderer(renderer_);
            auto temporal_composite=native_composite_.output();
            if(dlss_ && !eye_output && temporal_enabled_ && effects.temporal_background && std::getenv("STARFOX_TEST_DLSS_EVALUATE")) {
                const starfox::render::GpuModelDraw* projection=nullptr;bool consistent=true;
                for(const auto& item:temporal_draws_) if(const auto* draw=std::get_if<starfox::render::GpuModelDraw>(&item);draw && draw->identity) {
                    if(!projection) projection=draw;
                    else if(draw->pose.vanish_x!=projection->pose.vanish_x || draw->pose.vanish_y!=projection->pose.vanish_y || draw->settings.focal_length!=projection->settings.focal_length) consistent=false;
                }
                auto world=*effects.temporal_background;world.end_write_coverage();
                std::vector<std::uint8_t> world_coverage(frame.pixels().size());
                for(std::size_t i=0;i<world_coverage.size();++i) if(frame.write_coverage()[i] && frame.layer_tags()[i]!=1) {
                    world.pixels()[i]=frame.pixels()[i];world.layer_tags()[i]=frame.layer_tags()[i];world_coverage[i]=1;
                }
                const bool world_ready=projection && consistent && temporal_composite_.compose(source,source_scale,world,world_coverage,layer,palette,
                    has_late?&late_output:nullptr,effects.background?&background_output:nullptr,{},true);
                if(world_ready) if(const auto* path=std::getenv("STARFOX_TEST_DLSS_WORLD_CAPTURE")) {
                    const auto* frames=std::getenv("STARFOX_TEST_FRAMES");
                    if(frames && temporal_serial_==std::strtoull(frames,nullptr,10)) {
                        std::vector<std::uint8_t> rgba;
                        if(temporal_composite_.readback(world,rgba)) {
                            auto* image=SDL_CreateSurfaceFrom(world.stored_width(),world.stored_height(),SDL_PIXELFORMAT_RGBA32,rgba.data(),world.stored_width()*4);
                            if(image) {SDL_SaveBMP(image,path);SDL_DestroySurface(image);}
                        }
                    }
                }
                if(world_ready) temporal_composite=dlss_->evaluate(temporal_composite_.output(),
                    float(projection->settings.focal_length*frame.draw_scale()),
                    float((projection->pose.vanish_x+layer.offset_x)*frame.draw_scale()),
                    float((projection->pose.vanish_y+layer.offset_y)*frame.draw_scale()),temporal_serial_,temporal_epoch_,&temporal_composite,&effects.temporal_camera,
                    effects.temporal_ground?&*effects.temporal_ground:nullptr);
            }
            if(settings.presentation_texture && (!bloom_layer_ready_ || settings.presentation_glow_texture)
                && (!has_model_layer || settings.presentation_model_texture)
                && sdl_gpu_effects_.apply_resident(temporal_composite,frame,rgba_,settings)) {
                gpu_frame_pending_=true;
                if(effects.late_dust && std::getenv("STARFOX_TRACE_GPU"))
                    std::cerr<<"native-pipeline: GPU late margin stars\n";
                smooth_layer_ready_=has_model_layer;
                if(has_model_layer && std::getenv("STARFOX_TRACE_GPU") && !native_model_split_reported_) {
                    std::cerr<<"native-pipeline: GPU separated model layer\n";native_model_split_reported_=true;
                }
                if((settings.subtractive_overlays[0] || settings.subtractive_overlays[1])
                    && std::getenv("STARFOX_TRACE_GPU") && !native_planet_overlay_reported_) {
                    std::cerr<<"native-pipeline: GPU planet/briefing overlays\n";native_planet_overlay_reported_=true;
                }
                if(settings.setup_overlay && std::getenv("STARFOX_TRACE_GPU") && !native_setup_reported_) {
                    std::cerr<<"native-pipeline: GPU setup overlay\n";native_setup_reported_=true;
                }
                if(settings.touch_controls && std::getenv("STARFOX_TRACE_GPU") && !native_touch_reported_) {
                    std::cerr<<"native-pipeline: GPU touch controls\n";native_touch_reported_=true;
                }
                if(settings.confirmation_overlay && std::getenv("STARFOX_TRACE_GPU") && !native_confirmation_reported_) {
                    std::cerr<<"native-pipeline: GPU confirmation/slot overlay\n";
                    native_confirmation_reported_=true;
                }
                if(settings.host_overlay && std::getenv("STARFOX_TRACE_GPU") && !native_host_overlay_reported_) {
                    std::cerr<<"native-pipeline: GPU host FPS overlay\n";
                    native_host_overlay_reported_=true;
                }
                if(settings.window_mask && std::getenv("STARFOX_TRACE_GPU") && !native_window_mask_reported_) {
                    std::cerr<<"native-pipeline: GPU cartridge window mask\n";
                    native_window_mask_reported_=true;
                }
                if(settings.colour_math && std::getenv("STARFOX_TRACE_GPU") && !native_colour_math_reported_) {
                    std::cerr<<"native-pipeline: GPU fixed colour math\n";
                    native_colour_math_reported_=true;
                }
                if(settings.background_subtract && std::getenv("STARFOX_TRACE_GPU") && !native_background_fade_reported_) {
                    std::cerr<<"native-pipeline: GPU background fade\n";
                    native_background_fade_reported_=true;
                }
                if(settings.circle && std::getenv("STARFOX_TRACE_GPU") && !native_circle_reported_) {
                    std::cerr<<"native-pipeline: GPU bomb colour disk\n";
                    native_circle_reported_=true;
                }
                if(settings.horizontal_wipe && std::getenv("STARFOX_TRACE_GPU") && !native_wipe_reported_) {
                    std::cerr<<"native-pipeline: GPU horizontal scramble wipe\n";
                    native_wipe_reported_=true;
                }
                if(std::getenv("STARFOX_TRACE_GPU") && !native_direct_reported_) {
                    std::cerr<<"native-pipeline: resident raster -> composition -> effects -> presentation\n";
                    native_direct_reported_=true;
                }
                if(display_frame) present_rgba_pixels(frame.stored_width(),frame.stored_height(),rgba_,true);
                return true;
            }
        }
        // A deferred stereo eye must fail as a unit, never display a mono
        // fallback or advance the window between eyes.
        if(!display_frame || eye_output) return false;
        // Transitions/overlays not yet migrated still consume an authoritative
        // CPU frame. Reuse the completed GPU raster instead of rasterizing the
        // same models twice. Software replay is only a readback-failure fallback.
        // Restore every later foreground write, including same-colour writes.
        starfox::render::Framebuffer native(commands.width()/source_scale,commands.height()/source_scale,source_scale);
        native.enable_layer_tags(true);
        starfox::render::SurfaceBuffer surfaces(commands.width(),commands.height());
        if(recorded_scene_) {
            if(native_scene_.readback(native,&surfaces)) {
                if(std::getenv("STARFOX_TRACE_GPU") && !native_readback_reported_) {
                    std::cerr<<"native-pipeline: GPU scene readback for transition/overlay composition\n";
                    native_readback_reported_=true;
                }
            } else {
            if(std::getenv("STARFOX_TRACE_GPU")) {
                std::cerr<<"native-pipeline: CPU scene replay for transition/overlay composition\n";
            }
            recorded_scene_->replay(native,&surfaces);
            }
        } else if(!native_raster_.readback(native,
                native_raster_.resident_output().surfaces ? &surfaces : nullptr)) {
            starfox::render::replay_raster_commands(commands,native,&surfaces);
        } else if(std::getenv("STARFOX_TRACE_GPU") && !native_readback_reported_) {
            std::cerr<<"native-pipeline: GPU raster readback for CPU transition/overlay composition\n";
            native_readback_reported_=true;
        }
        auto composed=frame;composed.end_write_coverage();
        if(effects.background) restore_background(*effects.background,composed,effects.background_cpu_coverage);
        starfox::render::composite_transparent_layer(native,composed,layer);
        for(std::size_t i=0;i<frame.pixels().size();++i) if(frame.write_coverage()[i]) {
            composed.pixels()[i]=frame.pixels()[i];
            if(frame.layer_tags_enabled()) composed.layer_tags()[i]=frame.layer_tags()[i];
        }
        auto fallback=effects;fallback.model_surfaces=&surfaces;
        if(effects.late_cartridge) apply_late_cartridge(*effects.late_cartridge,composed);
        fallback.late_cartridge=nullptr;
        if(effects.background && effects.background->margin_origin && !effects.background->repair_margins) fill_frontend_margins(composed,effects.background->margin_origin);
        if(effects.late_dust) starfox::render::DustRenderer::draw_dust_frame(*effects.late_dust,composed);
        fallback.late_dust=nullptr;
        fallback.background=nullptr;fallback.background_cpu_coverage={};
        if(fallback.fixed_subtract_foreground) fallback.fixed_subtract_foreground=&native;
        present(composed,palette,circle,fallback);
        return true;
    }

    void present(
        const starfox::render::Framebuffer& framebuffer,
        std::span<const starfox::render::Rgba8> palette,
        const starfox::simulation::CircleEffectState& circle,
        const PresentationEffects& effects = {}) {
        window_scale_ = framebuffer.draw_scale();
        gpu_frame_pending_=false;
        ensure_dimensions(
            framebuffer.stored_width(), framebuffer.stored_height());
        starfox::render::expand_rgba(
            framebuffer, rgba_, palette, presentation_workers_);
        // Reconstruct the cartridge-authored 2D art before any screen-space
        // effect reads the frame, so anti-aliasing and the surface passes see
        // resolved edges rather than the nearest-neighbour blocks the render
        // scale produced. Declines by itself unless the framebuffer carries
        // layer tags; at native scale it reconstructs and resolves a 2x buffer.
        // STARFOX_2D_FILTER_DEBUG=1 paints every pixel the filter claims in
        // magenta, so a real frame shows directly which layer the runtime
        // thinks each part of it belongs to. Read once; it is a diagnostic,
        // not a setting.
        static const auto highlight_filtered = [] {
            const auto* value = std::getenv("STARFOX_2D_FILTER_DEBUG");
            return value != nullptr && std::string_view{value} != "0";
        }();
        starfox::render::GpuEffectSettings filter_gpu;
        filter_gpu.filter=static_cast<unsigned>(two_d_filter_);filter_gpu.highlight_filter=highlight_filtered;
        if (!apply_gpu_effects(framebuffer,filter_gpu))
            starfox::render::apply_two_d_filter(
                two_d_filter_, framebuffer, palette, rgba_, pixel_filter_scratch_,
                presentation_workers_, highlight_filtered);
        // Presentation effects address the source raster. Apply each one to
        // every stored pixel the render scale expanded that raster cell into.
        const auto render_scale = framebuffer.draw_scale();
        const auto stored_width =
            static_cast<std::size_t>(framebuffer.stored_width());
        const auto stored_pixel = [render_scale, stored_width](
                                      std::size_t x, std::size_t y,
                                      std::uint32_t column, std::uint32_t row) {
            return ((y * render_scale + row) * stored_width
                + x * render_scale + column) * 4U;
        };
        const auto composite_subtractive_overlay = [this, &framebuffer, palette,
                                                       &stored_pixel,
                                                       render_scale](
                                                       const auto* overlay_pointer,
                                                       std::uint8_t requested_brightness) {
            if (overlay_pointer == nullptr) return;
            const auto& overlay = *overlay_pointer;
            const auto brightness = std::min<std::uint32_t>(
                requested_brightness, 30U);
            const auto fixed_subtraction =
                static_cast<std::int32_t>(30U - brightness);
            const auto fade = [fixed_subtraction](std::uint32_t component) {
                const auto source_five = static_cast<std::int32_t>(
                    (component * 31U + 127U) / 255U);
                const auto result_five = std::max(
                    0, source_five - fixed_subtraction);
                return static_cast<std::uint8_t>(
                    (result_five << 3U) | (result_five >> 2U));
            };
            // These overlays composite in RGBA after the frame is expanded, so
            // they never reach the tagged framebuffer and the 2D filter cannot
            // see them. Filter the layer on its own and composite the result,
            // or the planet sequence's portraits stay blocky while the planet
            // behind them resolves.
            if (filter_overlay_gpu(overlay,palette,render_scale,overlay_argb_)
                || starfox::render::filter_overlay_layer(two_d_filter_, overlay,
                    palette, render_scale, overlay_argb_,
                    pixel_filter_scratch_, presentation_workers_)) {
                const auto stored_width =
                    static_cast<std::size_t>(framebuffer.stored_width());
                for (std::uint32_t y = 0; y < framebuffer.stored_height(); ++y) {
                    const auto* row = overlay_argb_.data()
                        + static_cast<std::size_t>(y) * stored_width;
                    for (std::uint32_t x = 0; x < stored_width; ++x) {
                        const auto colour = row[x];
                        const auto alpha = (colour >> 24U) & 0xffU;
                        if (alpha == 0U) continue;
                        const auto pixel =
                            (static_cast<std::size_t>(y) * stored_width + x)
                            * 4U;
                        const std::array<std::uint8_t, 3> faded{
                            fade((colour >> 16U) & 0xffU),
                            fade((colour >> 8U) & 0xffU),
                            fade(colour & 0xffU)};
                        if (alpha == 0xffU) {
                            rgba_[pixel] = faded[0];
                            rgba_[pixel + 1U] = faded[1];
                            rgba_[pixel + 2U] = faded[2];
                            continue;
                        }
                        // Partial coverage is the filter resolving an edge
                        // against the overlay's transparency; lay it over the
                        // frame underneath.
                        for (std::size_t channel = 0; channel < 3U; ++channel) {
                            rgba_[pixel + channel] = static_cast<std::uint8_t>(
                                (faded[channel] * alpha
                                    + rgba_[pixel + channel] * (255U - alpha)
                                    + 127U) / 255U);
                        }
                    }
                }
                return;
            }
            for (std::uint32_t y = 0; y < framebuffer.height(); ++y) {
                for (std::uint32_t x = 0; x < framebuffer.width(); ++x) {
                    const auto colour = overlay.get(x, y);
                    if (colour == 0U || colour >= palette.size()) continue;
                    const auto& source = palette[colour];
                    // PLANETS.ASM fades BG2 by subtracting a fixed white
                    // colour through CGADSUB. Multiplying RGB made the
                    // Pepper/Fox layer much too bright through most of the
                    // fade because every channel remained visible. Recreate
                    // the SNES five-bit subtraction instead.
                    const auto fixed = static_cast<std::int32_t>(30U - brightness);
                    const auto fade_component = [fixed](std::uint8_t component) {
                        const auto source_five = static_cast<std::int32_t>(
                            (static_cast<std::uint32_t>(component) * 31U + 127U)
                            / 255U);
                        const auto result_five = std::max(0, source_five - fixed);
                        return static_cast<std::uint8_t>(
                            (result_five << 3U) | (result_five >> 2U));
                    };
                    for (std::uint32_t block_row = 0;
                         block_row < render_scale;
                         ++block_row) {
                        for (std::uint32_t block_column = 0;
                             block_column < render_scale; ++block_column) {
                            const auto pixel = stored_pixel(
                                x, y, block_column, block_row);
                            rgba_[pixel] = fade_component(source.r);
                            rgba_[pixel + 1U] = fade_component(source.g);
                            rgba_[pixel + 2U] = fade_component(source.b);
                            rgba_[pixel + 3U] = source.a;
                        }
                    }
                }
            }
        };
        composite_subtractive_overlay(
            effects.overlay, effects.overlay_brightness);
        composite_subtractive_overlay(
            effects.text_overlay, effects.text_overlay_brightness);
        const auto subtract_fixed_white = [this](
                                              std::size_t pixel,
                                              std::uint8_t amount) {
            const auto fixed = static_cast<std::int32_t>(
                std::min<std::uint32_t>(amount, 31U));
            for (std::size_t component = 0; component < 3U; ++component) {
                const auto source_five = static_cast<std::int32_t>(
                    (static_cast<std::uint32_t>(rgba_[pixel + component])
                        * 31U + 127U) / 255U);
                const auto result_five = std::max(0, source_five - fixed);
                rgba_[pixel + component] = static_cast<std::uint8_t>(
                    (result_five << 3U) | (result_five >> 2U));
            }
        };
        if (effects.background_fixed_white_subtract != 0U) {
            for (std::int32_t y = 0;
                 y < static_cast<std::int32_t>(framebuffer.height()); ++y) {
                for (std::int32_t x = 0;
                     x < static_cast<std::int32_t>(framebuffer.width()); ++x) {
                    // GAMEOVER_L subtracts from BG2/BG3/backdrop but not the
                    // Super FX BG1 model layer or SNES OBJ sprites.
                    if (framebuffer.get(x, y) >= 128U) continue;
                    if (effects.fixed_subtract_foreground != nullptr) {
                        const auto mask_x = x
                            - effects.fixed_subtract_foreground_x;
                        const auto mask_y = y
                            - effects.fixed_subtract_foreground_y;
                        if (mask_x >= 0 && mask_y >= 0
                            && mask_x < static_cast<std::int32_t>(
                                effects.fixed_subtract_foreground->width())
                            && mask_y < static_cast<std::int32_t>(
                                effects.fixed_subtract_foreground->height())
                            && effects.fixed_subtract_foreground->get(
                                mask_x, mask_y) != 0U) {
                            continue;
                        }
                    }
                    for (std::uint32_t block_row = 0;
                         block_row < render_scale; ++block_row) {
                        for (std::uint32_t block_column = 0;
                             block_column < render_scale; ++block_column) {
                            const auto pixel = stored_pixel(
                                static_cast<std::size_t>(x),
                                static_cast<std::size_t>(y),
                                block_column, block_row);
                            subtract_fixed_white(pixel,
                                effects.background_fixed_white_subtract);
                        }
                    }
                }
            }
        }
        if (circle.active && circle.radius != 0U
            && (circle.affected_layers & 0x3fU) != 0U) {
            const auto expand_five = [](std::uint8_t value) {
                value &= 0x1fU;
                return static_cast<std::int32_t>((value << 3U) | (value >> 2U));
            };
            const auto brightness = std::min<std::int32_t>(
                effects.master_brightness, 15U);
            const std::array<std::int32_t, 3> fixed{
                expand_five(circle.red) * brightness / 15,
                expand_five(circle.green) * brightness / 15,
                expand_five(circle.blue) * brightness / 15,
            };
            const auto subtract = (circle.affected_layers & 0x80U) != 0U;
            const auto half = (circle.affected_layers & 0x40U) != 0U;
            // The bomb disk is geometry, not authored art: its edge comes from
            // a radius test, not from cartridge pixels. Testing once per source
            // cell and expanding the result is what makes it blocky, and no 2D
            // filter can recover a circle from that staircase. Evaluate the
            // same test per stored pixel instead, in stored units so the
            // comparison is identical at 1x and simply finer above it.
            const auto scale = static_cast<std::int64_t>(render_scale);
            const auto centre_x =
                static_cast<std::int64_t>(circle.centre_x) * scale;
            const auto centre_y =
                static_cast<std::int64_t>(circle.centre_y) * scale;
            const auto scaled_radius =
                static_cast<std::int64_t>(circle.radius) * scale;
            const auto radius_squared = scaled_radius * scaled_radius;
            const auto stored_width =
                static_cast<std::int64_t>(framebuffer.stored_width());
            const auto stored_height =
                static_cast<std::int64_t>(framebuffer.stored_height());
            // Walking the disk's bounding box rather than the whole frame also
            // makes this cheaper than the version it replaces.
            const auto first_y = std::max<std::int64_t>(
                0, centre_y - scaled_radius);
            const auto last_y = std::min<std::int64_t>(
                stored_height, centre_y + scaled_radius + 1);
            const auto first_x = std::max<std::int64_t>(
                0, centre_x - scaled_radius);
            const auto last_x = std::min<std::int64_t>(
                stored_width, centre_x + scaled_radius + 1);
            for (auto y = first_y; y < last_y; ++y) {
                const auto dy = y - centre_y;
                const auto dy_squared = dy * dy;
                if (dy_squared > radius_squared) continue;
                const auto source_y = static_cast<std::int32_t>(y / scale);
                for (auto x = first_x; x < last_x; ++x) {
                    const auto dx = x - centre_x;
                    if (dx * dx + dy_squared > radius_squared) continue;
                    const auto source_x = static_cast<std::int32_t>(x / scale);
                    // Clipping stays on the source raster, matching the PPU
                    // window it comes from.
                    if (effects.clip_circle
                        && (source_x < effects.circle_left
                            || source_x >= effects.circle_right
                            || source_y < effects.circle_top
                            || source_y >= effects.circle_bottom)) continue;
                    // CGADSUB bit 4 controls OBJ independently of BG1-4 and
                    // the backdrop. Star Fox's bomb program deliberately
                    // excludes sprites, so its HUD and communication OAM are
                    // not washed into the expanding disk. Reading the stored
                    // pixel rather than its cell's top-left corner also keeps
                    // the exclusion exact where the Super FX layer wrote at
                    // full resolution.
                    const auto source_index = framebuffer.get_stored(
                        static_cast<std::uint32_t>(x),
                        static_cast<std::uint32_t>(y));
                    if (source_index >= 128U
                        && (circle.affected_layers & 0x10U) == 0U) continue;
                    const auto pixel = (static_cast<std::size_t>(y)
                        * static_cast<std::size_t>(stored_width)
                        + static_cast<std::size_t>(x)) * 4U;
                    for (std::size_t component = 0;
                         component < 3U; ++component) {
                        const auto main = static_cast<std::int32_t>(
                            (static_cast<std::uint32_t>(
                                 rgba_[pixel + component])
                                * 31U + 127U) / 255U);
                        auto value = subtract
                            ? main - (fixed[component] >> 3U)
                            : main + (fixed[component] >> 3U);
                        if (half) value /= 2;
                        value = std::clamp(value, 0, 31);
                        rgba_[pixel + component] = static_cast<std::uint8_t>(
                            (value << 3U) | (value >> 2U));
                    }
                }
            }
        }
        starfox::render::apply_colour_math(effects.colour_math, framebuffer, rgba_);
        if (effects.planet.isolate_fade) {
            for (std::int32_t y = 0;
                 y < static_cast<std::int32_t>(framebuffer.height()); ++y) {
                for (std::int32_t x = 0;
                     x < static_cast<std::int32_t>(framebuffer.width()); ++x) {
                    if (x >= effects.planet.isolate_left
                        && x <= effects.planet.isolate_right
                        && y >= effects.planet.isolate_top
                        && y <= effects.planet.isolate_bottom) continue;
                    for (std::uint32_t block_row = 0;
                         block_row < render_scale; ++block_row) {
                        for (std::uint32_t block_column = 0;
                             block_column < render_scale; ++block_column) {
                            const auto pixel = stored_pixel(
                                static_cast<std::size_t>(x),
                                static_cast<std::size_t>(y),
                                block_column, block_row);
                            subtract_fixed_white(
                                pixel, effects.planet.isolate_amount);
                        }
                    }
                }
            }
        }
        if (effects.planet.level_fade) {
            for (std::size_t pixel = 0; pixel < rgba_.size(); pixel += 4U) {
                subtract_fixed_white(
                    pixel, effects.planet.level_fade_amount);
            }
        }
        if(effects.wipe.active && effects.wipe.horizontal_opening) {
            // Move the shutter's Y edges at output-pixel precision. The source
            // table switches entire rows; lerping its X bounds creates slits.
            const auto width=framebuffer.stored_width();
            for(std::uint32_t sy=0;sy<framebuffer.stored_height();++sy) {
                const double logical_y=(double(sy)+.5)/render_scale;
                const double source_y=effects.expand_wipe_vertical
                    ? logical_y*192.0/framebuffer.height() : logical_y-superfx_offset_y;
                if(source_y<0 || source_y>=192) continue;
                const bool closed=source_y<effects.wipe.opening_top
                    || source_y>=effects.wipe.opening_bottom;
                // MSCRAMWIPE keeps the first source column in its fixed guard.
                const auto end=closed || !effects.expand_wipe?width
                    :std::max(1U,(framebuffer.width()+221U)/223U)*render_scale;
                for(std::uint32_t x=0;x<end;++x) {
                    if(!closed && !effects.expand_wipe) {
                        const auto sx=int(x/render_scale)-int((framebuffer.width()-snes_width)/2);
                        if((!(sx>=15 && sx<=16))==(sx>=16 && sx<=240)) continue;
                    }
                    const auto i=(std::size_t(sy)*width+x)*4;
                    rgba_[i]=rgba_[i+1]=rgba_[i+2]=0;rgba_[i+3]=255;
                }
            }
        } else if (effects.wipe.active) {
            const auto origin_x = static_cast<std::int32_t>(
                framebuffer.width() > snes_width
                    ? (framebuffer.width() - snes_width) / 2U : 0U);
            const auto source_width = static_cast<std::int32_t>(
                framebuffer.width());
            const auto logic = static_cast<std::uint8_t>(
                effects.wipe.logic & 3U);
            const auto output_top = effects.expand_wipe_vertical
                ? 0 : superfx_offset_y;
            const auto output_bottom = effects.expand_wipe_vertical
                ? static_cast<std::int32_t>(framebuffer.height())
                : superfx_offset_y
                    + static_cast<std::int32_t>(effects.wipe.left.size());
            for (std::int32_t y = output_top; y < output_bottom; ++y) {
                if (y < 0 || y >= static_cast<std::int32_t>(
                        framebuffer.height())) continue;
                // The cartridge's colour-window table covers the 192-line
                // Super FX viewport. Gameplay is presented over all 224
                // output lines, so map that table over the complete host
                // raster during the launch reveal. Leaving the original
                // 16-line guards outside this loop made the top and bottom
                // of the world remain visible while the centre was closed.
                const auto line = effects.expand_wipe_vertical
                    ? static_cast<std::size_t>(std::clamp(
                        y * static_cast<std::int32_t>(
                            effects.wipe.left.size() - 1U)
                            / std::max(static_cast<std::int32_t>(
                                framebuffer.height()) - 1, 1),
                        0, static_cast<std::int32_t>(
                            effects.wipe.left.size() - 1U)))
                    : static_cast<std::size_t>(y - superfx_offset_y);
                const auto left = static_cast<std::uint8_t>(
                    effects.wipe.left[line]);
                const auto right = static_cast<std::uint8_t>(
                    effects.wipe.right[line]);
                for (std::int32_t x = 0;
                     x < static_cast<std::int32_t>(framebuffer.width()); ++x) {
                    // The source window tables describe the centred Super FX
                    // viewport. For a wide presentation, scale that active
                    // mask over the complete host scene too; merely centring
                    // it made the added columns enter/leave as black slabs.
                    const auto source_x = effects.expand_wipe
                        // Stretch the actual $10-$ef Super FX window, not
                        // the unused 16-pixel source guards. Mapping the host
                        // through $00-$ff made those guards become visible
                        // side slabs while the central shutter was black.
                        ? 16 + std::clamp(x * 223
                                / std::max(source_width - 1, 1), 0, 223)
                        : x - origin_x;
                    const auto inside_dynamic = left <= right
                        ? source_x >= left && source_x <= right
                        : source_x >= left || source_x <= right;
                    // W12SEL/W34SEL=$bb select the outside of dynamic window
                    // 1 and the inside of fixed viewport window 2 ($10-$f0).
                    const auto window_1 = !inside_dynamic;
                    const auto window_2 =
                        source_x >= 16 && source_x <= 240;
                    bool masked{};
                    switch (logic) {
                    case 1U: masked = window_1 && window_2; break; // AND
                    case 2U: masked = window_1 != window_2; break; // XOR
                    case 3U: masked = window_1 == window_2; break; // XNOR
                    case 0U:
                    default: masked = window_1 || window_2; break; // OR
                    }
                    if (!masked) continue;
                    for (std::uint32_t block_row = 0;
                         block_row < render_scale;
                         ++block_row) {
                        for (std::uint32_t block_column = 0;
                             block_column < render_scale; ++block_column) {
                            const auto pixel = stored_pixel(
                                static_cast<std::size_t>(x),
                                static_cast<std::size_t>(y),
                                block_column, block_row);
                            rgba_[pixel] = 0U;
                            rgba_[pixel + 1U] = 0U;
                            rgba_[pixel + 2U] = 0U;
                            rgba_[pixel + 3U] = 255U;
                        }
                    }
                }
            }
        }
        smooth_layer_ready_ = false;
        starfox::render::GpuEffectSettings early_gpu;
        early_gpu.hdr=effects.hdr_effect; early_gpu.chromatic=effects.chromatic_aberration;
        early_gpu.lighting=rtx_lighting_; early_gpu.surfaces=effects.model_surfaces;
        early_gpu.surface_x=effects.model_surface_x;early_gpu.surface_y=effects.model_surface_y;
        if(effects.shadow_mask || effects.resident_shadow.buffer) {
            if(effects.shadow_mask) early_gpu.shadow_mask=*effects.shadow_mask;
            early_gpu.resident_shadow=effects.resident_shadow;
            early_gpu.shadow_width=effects.shadow_width;early_gpu.shadow_height=effects.shadow_height;
            early_gpu.shadow_offset_y=effects.shadow_offset_y;
        }
        const bool early_on_gpu=apply_gpu_effects(framebuffer,early_gpu);
        if (!early_on_gpu) {
            if (rtx_lighting_) apply_rtx_lighting(framebuffer,effects);
            starfox::render::apply_hdr_effect(framebuffer,rgba_,effects.hdr_effect);
            starfox::render::apply_chromatic_aberration(framebuffer, rgba_,
                chromatic_scratch_, effects.chromatic_aberration);
        }
        std::vector<std::uint8_t> fallback_shadow;
        const auto* cpu_shadow=effects.shadow_mask;
        if(!early_on_gpu && effects.resident_shadow.buffer) {
            bool found=false;
            const auto read_owner=[&](auto& owner) {
                if(owner.output().buffer!=effects.resident_shadow.buffer) return;
                found=true;
                if(!owner.readback(fallback_shadow))
                    throw std::runtime_error("GPU shadow fallback readback failed: "+owner.status());
            };
            read_owner(resident_shadows_);read_owner(native_dxr_shadows_);
            for(auto& eye:stereo_resident_shadows_) read_owner(eye);
            for(auto& eye:stereo_native_dxr_shadows_) read_owner(eye);
            if(!found) throw std::runtime_error("GPU shadow fallback has no matching owner");
            cpu_shadow=&fallback_shadow;
        }
        if (!early_on_gpu && cpu_shadow != nullptr) {
            for (std::uint32_t y=0; y<framebuffer.stored_height(); ++y) {
                const auto sy=static_cast<int>(y)-effects.shadow_offset_y;
                if (sy<0 || sy>=static_cast<int>(effects.shadow_height)) continue;
                for (std::uint32_t x=0; x<framebuffer.stored_width(); ++x) {
                    const auto sx=x;
                    if (sx>=effects.shadow_width) continue;
                    const auto layer=framebuffer.layer_stored(x,y);
                    if (layer!=starfox::render::PixelLayer::background
                        && layer!=starfox::render::PixelLayer::three_d
                        && layer!=starfox::render::PixelLayer::textured_geometry) continue;
                    const auto shade=(*cpu_shadow)[static_cast<std::size_t>(sy)*effects.shadow_width+sx];
                    if (shade==0) continue;
                    const auto pixel=(static_cast<std::size_t>(y)*framebuffer.stored_width()+x)*4U;
                    for (unsigned channel=0; channel<3; ++channel)
                        rgba_[pixel+channel]=static_cast<std::uint8_t>(
                            static_cast<unsigned>(rgba_[pixel+channel])*(255U-shade)/255U);
                }
            }
        }
        if (effects.host_overlay != nullptr) {
            const auto& overlay = *effects.host_overlay;
            const auto paint = [this, &framebuffer, &stored_pixel,
                                   render_scale](
                                   std::int32_t x, std::int32_t y,
                                   std::uint8_t value) {
                if (x < 0 || y < 0
                    || x >= static_cast<std::int32_t>(framebuffer.width())
                    || y >= static_cast<std::int32_t>(framebuffer.height())) {
                    return;
                }
                for (std::uint32_t block_row = 0; block_row < render_scale;
                     ++block_row) {
                    for (std::uint32_t block_column = 0;
                         block_column < render_scale; ++block_column) {
                        const auto pixel = stored_pixel(
                            static_cast<std::size_t>(x),
                            static_cast<std::size_t>(y),
                            block_column, block_row);
                        rgba_[pixel] = value;
                        rgba_[pixel + 1U] = value;
                        rgba_[pixel + 2U] = value;
                        rgba_[pixel + 3U] = 255U;
                    }
                }
            };
            // Draw a one-pixel black shadow first, then opaque white glyphs.
            // This host diagnostic remains legible through every cartridge
            // palette, fade, bomb circle, and planet-isolation effect.
            for (std::uint32_t y = 0; y < overlay.height(); ++y) {
                for (std::uint32_t x = 0; x < overlay.width(); ++x) {
                    if (overlay.get(x, y) == 0U) continue;
                    paint(effects.host_overlay_x + static_cast<std::int32_t>(x) + 1,
                        effects.host_overlay_y + static_cast<std::int32_t>(y) + 1,
                        0U);
                }
            }
            for (std::uint32_t y = 0; y < overlay.height(); ++y) {
                for (std::uint32_t x = 0; x < overlay.width(); ++x) {
                    if (overlay.get(x, y) == 0U) continue;
                    paint(effects.host_overlay_x + static_cast<std::int32_t>(x),
                        effects.host_overlay_y + static_cast<std::int32_t>(y),
                        255U);
                }
            }
        }
        if (effects.confirmation_overlay != nullptr) {
            const auto& overlay = *effects.confirmation_overlay;
            const auto left = (static_cast<std::int32_t>(framebuffer.width())
                - static_cast<std::int32_t>(overlay.width())) / 2;
            const auto top = (static_cast<std::int32_t>(framebuffer.height())
                - static_cast<std::int32_t>(overlay.height())) / 2;
            const auto right = left + static_cast<std::int32_t>(overlay.width());
            const auto bottom = top + static_cast<std::int32_t>(overlay.height());
            const auto paint = [this, &framebuffer](
                                   std::int32_t x, std::int32_t y,
                                   std::uint8_t value) {
                if (x < 0 || y < 0
                    || x >= static_cast<std::int32_t>(framebuffer.width())
                    || y >= static_cast<std::int32_t>(framebuffer.height())) {
                    return;
                }
                const auto scale = framebuffer.draw_scale();
                const auto stored_width = static_cast<std::size_t>(
                    framebuffer.stored_width());
                const auto origin_x = static_cast<std::uint32_t>(x) * scale;
                const auto origin_y = static_cast<std::uint32_t>(y) * scale;
                for (std::uint32_t row = 0; row < scale; ++row) {
                    for (std::uint32_t column = 0; column < scale; ++column) {
                        const auto pixel = (static_cast<std::size_t>(
                            origin_y + row) * stored_width
                            + origin_x + column) * 4U;
                        rgba_[pixel] = value;
                        rgba_[pixel + 1U] = value;
                        rgba_[pixel + 2U] = value;
                        rgba_[pixel + 3U] = 255U;
                    }
                }
            };
            for (auto y = top - 4; y < bottom + 4; ++y) {
                for (auto x = left - 6; x < right + 6; ++x) {
                    const auto border = x == left - 6 || x == right + 5
                        || y == top - 4 || y == bottom + 3;
                    paint(x, y, border ? 255U : 0U);
                }
            }
            for (std::uint32_t y = 0U; y < overlay.height(); ++y) {
                for (std::uint32_t x = 0U; x < overlay.width(); ++x) {
                    if (overlay.get(x, y) == 0U) continue;
                    paint(left + static_cast<std::int32_t>(x),
                        top + static_cast<std::int32_t>(y), 255U);
                }
            }
        }
        starfox::render::GpuEffectSettings style_gpu;
        style_gpu.smoothing=model_smoothing_;
        style_gpu.model_effect=static_cast<unsigned>(effect_);
        style_gpu.world_effect=static_cast<unsigned>(world_effect_);
        style_gpu.model_intensity=effect_intensity_; style_gpu.world_intensity=world_effect_intensity_;
        bloom_layer_ready_ = bloom_ != 0U || bloom_2d_ != 0U;
        style_gpu.bloom_model=bloom_;style_gpu.bloom_world=bloom_2d_;
        style_gpu.anti_aliasing=static_cast<unsigned>(anti_aliasing_);
        if(bloom_layer_ready_) {
            style_gpu.bloom_base=&bloom_base_rgba_;style_gpu.bloom_glow=&bloom_glow_rgba_;
        }
        // No subsequent CPU composition is needed on this path. SDL samples
        // the compute result directly; screenshots/history read back on demand.
        if(renderer_mode_==starfox::simulation::RendererMode::gpu) {
            style_gpu.touch_controls=effects.touch_controls;
            if(effects.setup_overlay) style_gpu.setup_overlay=starfox::render::GpuEffectSettings::SetupOverlay{
                effects.setup_overlay,effects.setup_left,effects.setup_right,effects.setup_brightness};
            style_gpu.presentation_texture=effect_texture(texture_);
            if(bloom_layer_ready_ && style_gpu.presentation_texture) {
                ensure_bloom_texture(framebuffer.stored_width(),framebuffer.stored_height());
                style_gpu.presentation_glow_texture=effect_texture(bloom_texture_);
            }
            const bool has_model_layer=smooth_polys_ && effects.model_surfaces && !effects.model_surfaces->empty();
            if(has_model_layer && style_gpu.presentation_texture) {
                ensure_1440p_model_textures(framebuffer.stored_width(),framebuffer.stored_height());
                style_gpu.presentation_model_texture=effect_texture(smooth_model_texture_);
                style_gpu.surfaces=effects.model_surfaces;style_gpu.surface_x=effects.model_surface_x;
                style_gpu.surface_y=effects.model_surface_y;
            }
            if(style_gpu.presentation_texture && (!bloom_layer_ready_ || style_gpu.presentation_glow_texture)
                && (!has_model_layer || style_gpu.presentation_model_texture)
                && apply_gpu_effects(framebuffer,style_gpu)) {
                gpu_frame_pending_=true;
                smooth_layer_ready_=has_model_layer;
                if(std::getenv("STARFOX_TRACE_GPU") && !gpu_direct_reported_) {
                    std::cerr<<"gpu-presentation: direct "<<(portable_gpu_?"SDL GPU":"D3D11")<<"; bloom="<<bloom_layer_ready_
                        <<" model-layer="<<has_model_layer<<'\n';gpu_direct_reported_=true;
                }
                present_rgba_pixels(framebuffer.stored_width(),framebuffer.stored_height(),rgba_,true);
                return;
            }
            style_gpu.presentation_texture=nullptr;
            style_gpu.presentation_glow_texture=nullptr;
            style_gpu.presentation_model_texture=nullptr;
            // The fallback below composites these on CPU. Do not apply them
            // once in the uploaded style pass and then a second time below.
            style_gpu.setup_overlay.reset();style_gpu.touch_controls=false;
        }
        if (!apply_gpu_effects(framebuffer,style_gpu)) {
            starfox::render::smooth_models(model_smoothing_, framebuffer, rgba_, smoothing_scratch_, &presentation_workers_);
            starfox::render::apply_effect(effect_, framebuffer, rgba_, style_scratch_, effect_intensity_,
                world_effect_, world_effect_intensity_);
        if (bloom_layer_ready_) bloom_base_rgba_ = rgba_;
        bloom_pass_.apply(bloom_, bloom_2d_, framebuffer, rgba_, &presentation_workers_);
        if (bloom_layer_ready_) bloom_glow_rgba_ = rgba_;
        if (anti_aliasing_ != starfox::simulation::AntiAliasingMode::off) {
            apply_fxaa(anti_aliasing_);
        }
        }
        if (effects.setup_overlay) {
            const auto& overlay = *effects.setup_overlay;
            const auto origin = (framebuffer.width() - 256U) / 2U;
            for (std::uint32_t y = 0; y < framebuffer.stored_height(); ++y) {
                for (std::uint32_t x = 0; x < framebuffer.stored_width(); ++x) {
                    const auto sx = x / render_scale;
                    const auto sy = y / render_scale;
                    const auto i = (std::size_t(y) * framebuffer.stored_width() + x) * 4U;
                    const auto local_x = static_cast<std::int32_t>(sx) - static_cast<std::int32_t>(origin);
                    if (local_x >= effects.setup_left && local_x <= effects.setup_right && sy >= 20U && sy <= 222U) {
                        for (unsigned c = 0; c < 3; ++c) rgba_[i + c] /= 4U;
                    }
                    const auto ink = overlay.get(sx, sy);
                    if (ink == 0U) continue;
                    const auto colour = ink & 15U;
                    const auto rgb = colour == 14U ? std::array<std::uint8_t, 3>{255, 255, 255}
                        : colour == 10U ? std::array<std::uint8_t, 3>{255, 220, 64}
                        : std::array<std::uint8_t, 3>{180, 200, 215};
                    for (unsigned c = 0; c < 3; ++c)
                        rgba_[i + c] = static_cast<std::uint8_t>(rgb[c] * effects.setup_brightness / 15U);
                }
            }
        }
        if (effects.touch_controls) {
            apply_touch_controls(framebuffer.width(), framebuffer.height(),
                render_scale);
        }
        if (bloom_layer_ready_) {
            starfox::render::split_bloom_layer(bloom_base_rgba_, bloom_glow_rgba_, rgba_);
        }
        if (smooth_polys_) {
            if (bloom_layer_ready_) rgba_.swap(bloom_base_rgba_);
            prepare_1440p_model_layer(framebuffer, effects);
            if (bloom_layer_ready_) rgba_.swap(bloom_base_rgba_);
        }
        present_rgba_pixels(
            framebuffer.stored_width(), framebuffer.stored_height(), rgba_);
    }

    void present_rgba(std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> rgba) {
        gpu_frame_pending_=false;
        if (rgba.size() != static_cast<std::size_t>(width) * height * 4U) {
            throw std::invalid_argument{"RGBA presentation size mismatch"};
        }
        ensure_dimensions(width, height);
        smooth_layer_ready_ = false;
        bloom_layer_ready_ = false;
        rgba_.assign(rgba.begin(), rgba.end());
        present_rgba_pixels(width, height, rgba_);
    }

    [[nodiscard]] std::span<const std::uint8_t> rgba() {
        if(gpu_frame_pending_) {
            if(!(portable_gpu_?sdl_gpu_effects_.readback(rgba_):gpu_effects_.readback(rgba_)))
                throw std::runtime_error("GPU frame readback failed");
            gpu_frame_pending_=false;
        }
        return rgba_;
    }

    void set_render_options(
        starfox::simulation::RendererMode renderer_mode,
        starfox::simulation::AntiAliasingMode anti_aliasing,
        bool enhanced_graphics,
        bool smooth_polys, std::uint8_t rtx_lighting, bool vsync,
        starfox::render::TwoDFilter two_d_filter,
        starfox::render::Effect effect, std::uint8_t effect_intensity,
        starfox::render::Effect world_effect, std::uint8_t world_effect_intensity, std::uint8_t bloom, std::uint8_t bloom_2d, std::uint8_t model_smoothing) {
        bloom_ = bloom;
        bloom_2d_ = bloom_2d;
        model_smoothing_ = model_smoothing;
        effect_ = effect;
        effect_intensity_ = effect_intensity;
        world_effect_ = world_effect;
        world_effect_intensity_ = world_effect_intensity;
        if (renderer_mode_ != renderer_mode) {
            recreate_renderer(renderer_mode);
        }
        anti_aliasing_ = anti_aliasing;
        smooth_polys_ = smooth_polys;
        rtx_lighting_ = rtx_lighting;
        two_d_filter_ = two_d_filter;
        if (enhanced_graphics_ != enhanced_graphics) {
            enhanced_graphics_ = enhanced_graphics;
            if (texture_ != nullptr) {
                static_cast<void>(SDL_SetTextureScaleMode(texture_,
                    enhanced_graphics_ ? SDL_SCALEMODE_LINEAR
                                       : SDL_SCALEMODE_NEAREST));
            }
        }
        if (vsync_ != vsync) {
            vsync_ = vsync;
            static_cast<void>(SDL_SetRenderVSync(renderer_, vsync_ ? 1 : 0));
        }
    }

    void save_bmp(const std::filesystem::path& path) {
        static_cast<void>(rgba());
        auto* surface = SDL_CreateSurfaceFrom(
            texture_width_, texture_height_, SDL_PIXELFORMAT_RGBA32,
            const_cast<std::uint8_t*>(rgba_.data()), texture_width_ * 4U);
        if (surface == nullptr) {
            throw std::runtime_error{
                std::string{"SDL_CreateSurfaceFrom: "} + SDL_GetError()};
        }
        const auto path_text = path.string();
        const auto saved = SDL_SaveBMP(surface, path_text.c_str());
        SDL_DestroySurface(surface);
        if (!saved) {
            throw std::runtime_error{
                std::string{"SDL_SaveBMP: "} + SDL_GetError()};
        }
    }

    void set_relative_mouse_mode(bool enabled) noexcept {
        if (relative_mouse_mode_ == enabled) return;
        if (SDL_SetWindowRelativeMouseMode(window_, enabled)) {
            relative_mouse_mode_ = enabled;
        }
    }

    void toggle_fullscreen() {
#if defined(__ANDROID__)
        constexpr bool enter_fullscreen = true;
#else
        const auto enter_fullscreen =
            (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0U;
#endif
        if (!SDL_SetWindowFullscreen(window_, enter_fullscreen)) {
            throw std::runtime_error{
                std::string{"SDL_SetWindowFullscreen: "} + SDL_GetError()};
        }
        static_cast<void>(SDL_SyncWindow(window_));
        if (!enter_fullscreen) {
            set_windowed_size(texture_width_, texture_height_);
            static_cast<void>(SDL_SyncWindow(window_));
        }
    }

    [[nodiscard]] bool window_to_logical(
        float window_x, float window_y,
        float& logical_x, float& logical_y) const noexcept {
        if(!SDL_RenderCoordinatesFromWindow(
            renderer_, window_x, window_y, &logical_x, &logical_y)) return false;
        logical_x=starfox::render::presentation_to_raster_x(
            logical_x,texture_width_,texture_height_);
        return true;
    }

    void set_frame_debug_status(
        bool frozen, std::size_t cursor = 0U, std::size_t count = 0U) noexcept {
        constexpr std::string_view base_title =
            "Star Fox Enhanced - native PC runtime";
        temporary_status_until_.reset();
        if (!frozen) {
            static_cast<void>(SDL_SetWindowTitle(window_, base_title.data()));
            return;
        }
        const auto title = std::string{base_title} + " [FROZEN "
            + std::to_string(count == 0U ? 0U : cursor + 1U) + "/"
            + std::to_string(count) + "]";
        static_cast<void>(SDL_SetWindowTitle(window_, title.c_str()));
    }

    void show_temporary_status(std::string_view status) {
        constexpr std::string_view base_title =
            "Star Fox Enhanced - native PC runtime";
        const auto title = std::string{base_title} + " [" + std::string{status}
            + "]";
        static_cast<void>(SDL_SetWindowTitle(window_, title.c_str()));
        temporary_status_until_ = std::chrono::steady_clock::now()
            + std::chrono::seconds{2};
    }

    void update_temporary_status() noexcept {
        if (!temporary_status_until_
            || std::chrono::steady_clock::now() < *temporary_status_until_) {
            return;
        }
        temporary_status_until_.reset();
        static_cast<void>(SDL_SetWindowTitle(window_,
            "Star Fox Enhanced - native PC runtime"));
    }

private:
    void* effect_texture(SDL_Texture* texture) const {
        return SDL_GetPointerProperty(SDL_GetTextureProperties(texture),portable_gpu_
            ? SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER:SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER,nullptr);
    }
    void* effect_device() const {
        return SDL_GetPointerProperty(SDL_GetRendererProperties(renderer_),portable_gpu_
            ? SDL_PROP_RENDERER_GPU_DEVICE_POINTER:SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,nullptr);
    }
    bool run_gpu_effects(const starfox::render::Framebuffer& frame,std::vector<std::uint8_t>& pixels,
        const starfox::render::GpuEffectSettings& settings) {
        SDL_FlushRenderer(renderer_);
        const bool ok=portable_gpu_?sdl_gpu_effects_.apply(effect_device(),frame,pixels,settings)
            :gpu_effects_.apply(effect_device(),frame,pixels,settings);
        if(!ok && portable_gpu_ && frame.layer_tags_enabled() && effect_device() && !gpu_fallback_reported_) {
            std::cerr<<"GPU effects fallback: "<<sdl_gpu_effects_.status()<<'\n';gpu_fallback_reported_=true;
        }
        return ok;
    }
    bool filter_overlay_gpu(const starfox::render::Framebuffer& overlay,
        std::span<const starfox::render::Rgba8> palette,unsigned scale,
        std::vector<std::uint32_t>& output) {
        if(two_d_filter_==starfox::render::TwoDFilter::off || palette.empty()
            || renderer_mode_!=starfox::simulation::RendererMode::gpu
            || std::getenv("STARFOX_DISABLE_GPU_EFFECTS")) return false;
        auto* device=effect_device();
        if(!device) return false;
        starfox::render::Framebuffer frame(overlay.width(),overlay.height(),scale);
        frame.enable_layer_tags(true);
        std::vector<std::uint8_t> pixels(frame.pixels().size()*4,0);
        for(unsigned y=0;y<overlay.height();++y) for(unsigned x=0;x<overlay.width();++x) {
            const auto entry=overlay.get(x,y);
            if(!entry || entry>=palette.size()) continue;
            const auto i=(std::size_t(y)*frame.stored_width()+x)*4;
            pixels[i]=palette[entry].r;pixels[i+1]=palette[entry].g;
            pixels[i+2]=palette[entry].b;pixels[i+3]=255;
        }
        starfox::render::GpuEffectSettings settings;
        settings.filter=static_cast<unsigned>(two_d_filter_);settings.overlay_filter=true;
        SDL_FlushRenderer(renderer_);
        if(!run_gpu_effects(frame,pixels,settings)) return false;
        output.resize(frame.pixels().size());
        for(std::size_t i=0;i<output.size();++i)
            output[i]=(std::uint32_t(pixels[i*4+3])<<24)|(std::uint32_t(pixels[i*4])<<16)
                |(std::uint32_t(pixels[i*4+1])<<8)|pixels[i*4+2];
        return true;
    }
    bool apply_gpu_effects(const starfox::render::Framebuffer& frame,
        const starfox::render::GpuEffectSettings& settings) {
        if(renderer_mode_!=starfox::simulation::RendererMode::gpu
            || std::getenv("STARFOX_DISABLE_GPU_EFFECTS")) return false;
        if(!settings.hdr && !settings.chromatic && !settings.smoothing
            && !settings.model_effect && !settings.world_effect && !settings.anti_aliasing && !settings.lighting
            && !settings.bloom_model && !settings.bloom_world && !settings.filter && settings.shadow_mask.empty()
            && !settings.resident_shadow.buffer && !settings.presentation_texture
            && !settings.horizontal_wipe && !settings.circle && !settings.background_subtract
            && !settings.colour_math && !settings.planet_fade && !settings.window_mask
            && !settings.host_overlay && !settings.confirmation_overlay && !settings.setup_overlay
            && !settings.touch_controls && !settings.subtractive_overlays[0] && !settings.subtractive_overlays[1]) return true;
        return run_gpu_effects(frame,rgba_,settings);
    }
    void recreate_renderer(starfox::simulation::RendererMode mode) {
        if(renderer_ && dlss_) dlss_->finish(renderer_);
        temporal_composite_.release_device();
        reset_temporal_history();
        gpu_effects_.release_device();
        sdl_gpu_effects_.release_device();
        native_composite_.release_device();
        native_raster_.release_device();
        native_scene_.release_device();native_stereo_scene_.release_device();release_stereo_textures();recorded_scene_=nullptr;
        resident_shadows_.release_device();
        for(auto& shadows:stereo_resident_shadows_) shadows.release_device();
        native_dxr_shadows_.release_device();
        for(auto& shadows:stereo_native_dxr_shadows_) shadows.release_device();
        native_shadow_selected_=false;stereo_native_shadow_selected_.fill(false);
        portable_gpu_=false;gpu_fallback_reported_=false;gpu_direct_reported_=false;
        gpu_frame_pending_=false;
        const auto replace_window = renderer_ != nullptr
            && renderer_mode_ == starfox::simulation::RendererMode::gpu
            && mode == starfox::simulation::RendererMode::software;
        SDL_DestroyTexture(smooth_model_texture_);
        SDL_DestroyTexture(smooth_target_texture_);
        SDL_DestroyTexture(texture_);
        SDL_DestroyTexture(bloom_texture_);
        bloom_texture_ = nullptr;
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
        smooth_model_texture_ = nullptr;
        smooth_target_texture_ = nullptr;
        texture_ = nullptr;
#if defined(_WIN32) && !defined(STARFOX_UWP)
        if (replace_window) window_ = recreate_software_window(window_);
#else
        (void)replace_window;
#endif
#if defined(STARFOX_UWP)
        // Xbox UWP exposes SDL through its WinRT/D3D11 video backend. Avoid
        // automatic probing of desktop-only drivers during activation.
        const auto* renderer_driver =
            mode == starfox::simulation::RendererMode::software
                ? "software" : "direct3d11";
#elif defined(_WIN32) && defined(STARFOX_SDL_GPU_EFFECTS)
        // Preserve the established Vulkan default while D3D12 receives broader
        // live parity coverage. Explicit backend overrides are supported.
        SDL_SetHintWithPriority(SDL_HINT_GPU_DRIVER, "vulkan", SDL_HINT_DEFAULT);
        const auto* renderer_driver = mode == starfox::simulation::RendererMode::software
            ? "software" : std::string_view(SDL_GetCurrentVideoDriver())=="dummy"?nullptr:"gpu";
#elif defined(_WIN32)
        const auto* renderer_driver = mode == starfox::simulation::RendererMode::software
            ? "software" : std::string_view(SDL_GetCurrentVideoDriver())=="dummy"?nullptr:
                std::getenv("STARFOX_TEST_SDL_GPU")?"gpu":"direct3d11";
#elif defined(STARFOX_SDL_GPU_EFFECTS)
        const auto* renderer_driver = mode == starfox::simulation::RendererMode::software
            ? "software" : std::string_view(SDL_GetCurrentVideoDriver())=="dummy"?nullptr:"gpu";
#else
        const auto* renderer_driver =
            mode == starfox::simulation::RendererMode::software
                ? "software" : nullptr;
#endif
        bool gpu_hardware_requested=false;
#if defined(STARFOX_SDL_GPU_EFFECTS)
        if(renderer_driver && std::string_view(renderer_driver)=="gpu") {
            const auto props=SDL_CreateProperties();
            if(!props) throw std::runtime_error(SDL_GetError());
            const bool hardware=std::getenv("STARFOX_TEST_SOFTWARE_GPU")==nullptr;
            const bool configured=SDL_SetPointerProperty(props,SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,window_)
                && SDL_SetStringProperty(props,SDL_PROP_RENDERER_CREATE_NAME_STRING,renderer_driver)
                && SDL_SetBooleanProperty(props,SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,hardware);
            const bool interop=configured && starfox::render::shadows::SdlDxrShadows::request_vulkan_interop(props);
            renderer_=configured?SDL_CreateRendererWithProperties(props):nullptr;
            if(!renderer_ && interop) {
                SDL_ClearProperty(props,SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER);
                renderer_=SDL_CreateRendererWithProperties(props);
            }
            SDL_DestroyProperties(props);
            gpu_hardware_requested=hardware && renderer_!=nullptr;
        } else
#endif
        renderer_ = SDL_CreateRenderer(window_, renderer_driver);
        if(!renderer_ && renderer_driver && std::string_view(renderer_driver)=="gpu") {
            std::cerr<<"SDL GPU unavailable: "<<SDL_GetError()<<"; using native renderer fallback\n";
            renderer_=SDL_CreateRenderer(window_,nullptr);
        }
        if (renderer_ == nullptr) {
            throw std::runtime_error{
                std::string{"SDL_CreateRenderer: "} + SDL_GetError()};
        }
        renderer_mode_ = mode;
        portable_gpu_=SDL_GetPointerProperty(SDL_GetRendererProperties(renderer_),SDL_PROP_RENDERER_GPU_DEVICE_POINTER,nullptr)!=nullptr;
        native_gpu_binning_=false;
#if defined(STARFOX_SDL_GPU_EFFECTS)
        if(gpu_hardware_requested && portable_gpu_ && !std::getenv("STARFOX_DISABLE_GPU_BINS"))
            native_gpu_binning_=std::string_view(SDL_GetGPUDeviceDriver(static_cast<SDL_GPUDevice*>(effect_device())))=="vulkan";
#else
        (void)gpu_hardware_requested;
#endif
        // Presentation has its own exact schedule. Following an arbitrary
        // desktop refresh is opt-in through the saved VSYNC menu choice.
        static_cast<void>(SDL_SetRenderVSync(renderer_, vsync_ ? 1 : 0));
        if (!SDL_SetRenderLogicalPresentation(renderer_,
                static_cast<int>(starfox::render::presentation_width(texture_width_,texture_height_)),
                static_cast<int>(texture_height_),
                SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
            throw std::runtime_error{
                std::string{"SDL_SetRenderLogicalPresentation: "}
                + SDL_GetError()};
        }
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, static_cast<int>(texture_width_),
            static_cast<int>(texture_height_));
        if (texture_ == nullptr) {
            throw std::runtime_error{
                std::string{"SDL_CreateTexture: "} + SDL_GetError()};
        }
        static_cast<void>(SDL_SetTextureScaleMode(texture_,
            enhanced_graphics_ ? SDL_SCALEMODE_LINEAR
                               : SDL_SCALEMODE_NEAREST));
        // Present black while the next game frame is being prepared.
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        if (!SDL_RenderClear(renderer_)) {
            throw std::runtime_error{
                std::string{"SDL_RenderClear after renderer switch: "}
                + SDL_GetError()};
        }
        SDL_RenderPresent(renderer_);
        static_cast<void>(SDL_SyncWindow(window_));
        smooth_layer_ready_ = false;
        smooth_source_width_ = 0U;
        smooth_source_height_ = 0U;
        smooth_target_width_ = 0U;
    }

    void apply_touch_controls(std::uint32_t width, std::uint32_t height,
        std::uint32_t draw_scale) {
        draw_scale = std::max(1U, draw_scale);
        const auto stored_width = static_cast<std::size_t>(width) * draw_scale;
        const auto blend = [this, width, height, draw_scale, stored_width](
                               std::int32_t x, std::int32_t y,
                               std::uint8_t red, std::uint8_t green,
                               std::uint8_t blue, std::uint8_t alpha) {
            if (x < 0 || y < 0
                || x >= static_cast<std::int32_t>(width)
                || y >= static_cast<std::int32_t>(height)) return;
            const auto inverse = static_cast<std::uint32_t>(255U - alpha);
            for (std::uint32_t row = 0U; row < draw_scale; ++row) {
                for (std::uint32_t column = 0U; column < draw_scale; ++column) {
                    const auto pixel = ((static_cast<std::size_t>(y)
                            * draw_scale + row) * stored_width
                        + static_cast<std::size_t>(x) * draw_scale + column)
                        * 4U;
                    rgba_[pixel] = static_cast<std::uint8_t>(
                        (rgba_[pixel] * inverse + red * alpha + 127U) / 255U);
                    rgba_[pixel + 1U] = static_cast<std::uint8_t>(
                        (rgba_[pixel + 1U] * inverse
                            + green * alpha + 127U) / 255U);
                    rgba_[pixel + 2U] = static_cast<std::uint8_t>(
                        (rgba_[pixel + 2U] * inverse
                            + blue * alpha + 127U) / 255U);
                }
            }
        };
        const auto box = [&blend](std::int32_t left, std::int32_t top,
                                  std::int32_t right, std::int32_t bottom,
                                  std::uint8_t red = 235U,
                                  std::uint8_t green = 245U,
                                  std::uint8_t blue = 255U) {
            for (auto y = top; y <= bottom; ++y) {
                for (auto x = left; x <= right; ++x) {
                    const auto edge = x == left || x == right
                        || y == top || y == bottom;
                    blend(x, y, edge ? red : 18U, edge ? green : 28U,
                        edge ? blue : 42U, edge ? 170U : 76U);
                }
            }
        };
        const auto w = static_cast<std::int32_t>(width);
        const auto h = static_cast<std::int32_t>(height);
        const auto dpad_x = w * 20 / 100;
        const auto dpad_y = h * 73 / 100;
        const auto unit = std::max(7, h / 28);
        box(dpad_x - unit, dpad_y - unit * 3,
            dpad_x + unit, dpad_y - unit);
        box(dpad_x - unit, dpad_y + unit,
            dpad_x + unit, dpad_y + unit * 3);
        box(dpad_x - unit * 3, dpad_y - unit,
            dpad_x - unit, dpad_y + unit);
        box(dpad_x + unit, dpad_y - unit,
            dpad_x + unit * 3, dpad_y + unit);
        box(dpad_x - unit, dpad_y - unit,
            dpad_x + unit, dpad_y + unit, 150U, 180U, 210U);

        const auto action = [&](std::int32_t percent_x,
                                std::int32_t percent_y,
                                std::uint8_t red, std::uint8_t green,
                                std::uint8_t blue) {
            const auto cx = w * percent_x / 100;
            const auto cy = h * percent_y / 100;
            box(cx - unit, cy - unit, cx + unit, cy + unit,
                red, green, blue);
        };
        action(89, 69, 100U, 235U, 120U);
        action(77, 81, 245U, 105U, 105U);
        action(77, 57, 100U, 155U, 255U);
        action(65, 69, 250U, 220U, 95U);
        box(7, 7, w * 30 / 100, 20, 205U, 215U, 230U);
        box(w * 70 / 100, 7, w - 8, 20, 205U, 215U, 230U);
        box(w * 36 / 100, h - 20, w * 47 / 100, h - 7,
            205U, 215U, 230U);
        box(w * 53 / 100, h - 20, w * 64 / 100, h - 7,
            205U, 215U, 230U);
    }

    void prepare_1440p_model_layer(
        const starfox::render::Framebuffer& framebuffer,
        const PresentationEffects& effects) {
        if (effects.model_surfaces == nullptr
            || effects.model_surfaces->empty()) return;
        const auto width = static_cast<std::size_t>(framebuffer.stored_width());
        const auto height = static_cast<std::size_t>(framebuffer.stored_height());
        const auto pixels = width * height;
        smooth_base_rgba_ = rgba_;
        smooth_model_rgba_.assign(rgba_.size(), 0U);
        smooth_model_mask_.assign(pixels, 0U);

        const auto first_x = std::max(0, effects.model_surface_x
            + static_cast<std::int32_t>(effects.model_surfaces->minimum_x()));
        const auto first_y = std::max(0, effects.model_surface_y
            + static_cast<std::int32_t>(effects.model_surfaces->minimum_y()));
        const auto last_x = std::min(
            static_cast<std::int32_t>(framebuffer.stored_width()),
            effects.model_surface_x
                + static_cast<std::int32_t>(
                    effects.model_surfaces->maximum_x()) + 1);
        const auto last_y = std::min(
            static_cast<std::int32_t>(framebuffer.stored_height()),
            effects.model_surface_y
                + static_cast<std::int32_t>(
                    effects.model_surfaces->maximum_y()) + 1);
        for (auto y = first_y; y < last_y; ++y) {
            for (auto x = first_x; x < last_x; ++x) {
                if (model_surface_at(framebuffer, effects, x, y) == nullptr) {
                    continue;
                }
                const auto index = static_cast<std::size_t>(y) * width
                    + static_cast<std::size_t>(x);
                const auto rgba_index = index * 4U;
                smooth_model_mask_[index] = 1U;
                std::copy_n(rgba_.begin()
                        + static_cast<std::ptrdiff_t>(rgba_index),
                    4, smooth_model_rgba_.begin()
                        + static_cast<std::ptrdiff_t>(rgba_index));
                smooth_model_rgba_[rgba_index + 3U] = 255U;
            }
        }

        // Remove only the one-pixel source silhouette from the base layer.
        // The linearly sampled model texture then owns those pixels at 1440p,
        // including fractional-alpha edge coverage, without leaving the old
        // low-resolution stair-step underneath it. Opaque interior pixels do
        // not need reconstruction and remain hidden by the model layer.
        constexpr std::array<std::array<std::int32_t, 2>, 8> neighbours{{
            {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
            {{-1, -1}}, {{1, -1}}, {{-1, 1}}, {{1, 1}},
        }};
        for (auto y = first_y; y < last_y; ++y) {
            for (auto x = first_x; x < last_x; ++x) {
                const auto index = static_cast<std::size_t>(y) * width
                    + static_cast<std::size_t>(x);
                if (smooth_model_mask_[index] == 0U) continue;
                std::optional<std::size_t> background;
                for (const auto& offset : neighbours) {
                    const auto nx = x + offset[0];
                    const auto ny = y + offset[1];
                    if (nx < 0 || ny < 0
                        || nx >= static_cast<std::int32_t>(width)
                        || ny >= static_cast<std::int32_t>(height)) continue;
                    const auto neighbour = static_cast<std::size_t>(ny) * width
                        + static_cast<std::size_t>(nx);
                    if (smooth_model_mask_[neighbour] == 0U) {
                        background = neighbour;
                        break;
                    }
                }
                if (!background) continue;
                std::copy_n(rgba_.begin()
                        + static_cast<std::ptrdiff_t>(*background * 4U),
                    4, smooth_base_rgba_.begin()
                        + static_cast<std::ptrdiff_t>(index * 4U));
            }
        }
        smooth_layer_ready_ = true;
    }

    void ensure_1440p_model_textures(
        std::uint32_t width, std::uint32_t height) {
        constexpr std::uint32_t target_height = 1440U;
        const auto target_width = static_cast<std::uint32_t>(std::llround(
            static_cast<double>(width) * target_height
            / static_cast<double>(height)));
        if (smooth_model_texture_ != nullptr
            && smooth_target_texture_ != nullptr
            && smooth_source_width_ == width
            && smooth_source_height_ == height
            && smooth_target_width_ == target_width) return;
        SDL_DestroyTexture(smooth_model_texture_);
        SDL_DestroyTexture(smooth_target_texture_);
        smooth_model_texture_ = SDL_CreateTexture(renderer_,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(width), static_cast<int>(height));
        smooth_target_texture_ = SDL_CreateTexture(renderer_,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(target_width), static_cast<int>(target_height));
        if (smooth_model_texture_ == nullptr
            || smooth_target_texture_ == nullptr) {
            throw std::runtime_error{
                std::string{"SDL_CreateTexture (1440p model target): "}
                + SDL_GetError()};
        }
        static_cast<void>(SDL_SetTextureScaleMode(
            smooth_model_texture_, SDL_SCALEMODE_LINEAR));
        static_cast<void>(SDL_SetTextureBlendMode(
            smooth_model_texture_, SDL_BLENDMODE_BLEND));
        static_cast<void>(SDL_SetTextureScaleMode(
            smooth_target_texture_, SDL_SCALEMODE_LINEAR));
        smooth_source_width_ = width;
        smooth_source_height_ = height;
        smooth_target_width_ = target_width;
    }

    [[nodiscard]] static std::uint16_t luma(
        std::span<const std::uint8_t> pixels, std::size_t pixel) noexcept {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(pixels[pixel]) * 77U
                + static_cast<std::uint32_t>(pixels[pixel + 1U]) * 150U
                + static_cast<std::uint32_t>(pixels[pixel + 2U]) * 29U)
            >> 8U);
    }

    // Coordinates here are stored-raster, not source-raster. Scan conversion
    // writes both the indexed pixel and its surface sample at the render
    // scale, so the effect passes address them the same way and resolve
    // polygon edges at whatever scale the frame was drawn at.
    [[nodiscard]] static const starfox::render::SurfaceSample* model_surface_at(
        const starfox::render::Framebuffer& framebuffer,
        const PresentationEffects& effects,
        std::int32_t x,
        std::int32_t y) noexcept {
        if (effects.model_surfaces == nullptr) return nullptr;
        const auto local_x = x - effects.model_surface_x;
        const auto local_y = y - effects.model_surface_y;
        if (local_x < 0 || local_y < 0
            || local_x >= static_cast<std::int32_t>(
                effects.model_surfaces->width())
            || local_y >= static_cast<std::int32_t>(
                effects.model_surfaces->height())
            || x < 0 || y < 0
            || x >= static_cast<std::int32_t>(framebuffer.stored_width())
            || y >= static_cast<std::int32_t>(framebuffer.stored_height())) {
            return nullptr;
        }
        const auto& sample = effects.model_surfaces->get(
            static_cast<std::uint32_t>(local_x),
            static_cast<std::uint32_t>(local_y));
        // A later particle, HUD element, or cartridge layer may cover the
        // polygon. Only shade the surface if its indexed colour still owns the
        // final composite pixel.
        if (!sample.valid || framebuffer.get_stored(
                static_cast<std::uint32_t>(x),
                static_cast<std::uint32_t>(y)) != sample.palette_index) {
            return nullptr;
        }
        return &sample;
    }

    void capture_effect_source_region(
        std::int32_t first_x, std::int32_t first_y,
        std::int32_t last_x, std::int32_t last_y) {
        first_x = std::clamp(first_x, 0,
            static_cast<std::int32_t>(texture_width_));
        first_y = std::clamp(first_y, 0,
            static_cast<std::int32_t>(texture_height_));
        last_x = std::clamp(last_x, first_x,
            static_cast<std::int32_t>(texture_width_));
        last_y = std::clamp(last_y, first_y,
            static_cast<std::int32_t>(texture_height_));
        effect_source_x_ = first_x;
        effect_source_y_ = first_y;
        effect_source_width_ = static_cast<std::size_t>(last_x - first_x);
        const auto height = static_cast<std::size_t>(last_y - first_y);
        effect_source_.resize(effect_source_width_ * height * 4U);
        const auto source_width = static_cast<std::size_t>(texture_width_);
        for (std::size_t row = 0U; row < height; ++row) {
            const auto source = ((static_cast<std::size_t>(first_y) + row)
                * source_width + static_cast<std::size_t>(first_x)) * 4U;
            std::copy_n(rgba_.begin() + static_cast<std::ptrdiff_t>(source),
                static_cast<std::ptrdiff_t>(effect_source_width_ * 4U),
                effect_source_.begin()
                    + static_cast<std::ptrdiff_t>(row * effect_source_width_ * 4U));
        }
    }

    [[nodiscard]] std::size_t effect_source_pixel(
        std::int32_t x, std::int32_t y) const noexcept {
        return (static_cast<std::size_t>(y - effect_source_y_)
            * effect_source_width_
            + static_cast<std::size_t>(x - effect_source_x_)) * 4U;
    }

    void apply_smooth_polygons(
        const starfox::render::Framebuffer& framebuffer,
        const PresentationEffects& effects) {
        if (effects.model_surfaces == nullptr
            || effects.model_surfaces->empty()
            || framebuffer.stored_width() < 3U
            || framebuffer.stored_height() < 3U) {
            return;
        }
        const auto width = static_cast<std::size_t>(framebuffer.stored_width());
        const auto first_x = std::max(1, effects.model_surface_x
            + static_cast<std::int32_t>(effects.model_surfaces->minimum_x()) - 1);
        const auto first_y = std::max(1, effects.model_surface_y
            + static_cast<std::int32_t>(effects.model_surfaces->minimum_y()) - 1);
        const auto last_x = std::min(
            static_cast<std::int32_t>(framebuffer.stored_width()) - 1,
            effects.model_surface_x
                + static_cast<std::int32_t>(effects.model_surfaces->maximum_x()) + 1);
        const auto last_y = std::min(
            static_cast<std::int32_t>(framebuffer.stored_height()) - 1,
            effects.model_surface_y
                + static_cast<std::int32_t>(effects.model_surfaces->maximum_y()) + 1);
        capture_effect_source_region(
            first_x - 1, first_y - 1, last_x + 1, last_y + 1);
        constexpr std::array<std::array<std::int32_t, 2>, 9> samples{{
            {{0, 0}}, {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
            {{-1, -1}}, {{1, -1}}, {{-1, 1}}, {{1, 1}},
        }};
        for (auto y = first_y; y < last_y; ++y) {
            for (auto x = first_x; x < last_x; ++x) {
                const auto pixel = (static_cast<std::size_t>(y) * width
                    + static_cast<std::size_t>(x)) * 4U;
                const auto* centre = model_surface_at(framebuffer, effects, x, y);
                std::array<std::uint32_t, 3> model_total{};
                std::array<std::uint32_t, 3> background_total{};
                std::array<std::uint32_t, 3> crease_total{};
                auto model_count = 0U;
                auto background_count = 0U;
                auto crease_count = 0U;
                for (const auto& offset : samples) {
                    const auto sample_x = x + offset[0];
                    const auto sample_y = y + offset[1];
                    const auto sample_pixel = effect_source_pixel(
                        sample_x, sample_y);
                    const auto* surface = model_surface_at(
                        framebuffer, effects, sample_x, sample_y);
                    if (surface == nullptr) {
                        for (std::size_t component = 0U;
                             component < 3U; ++component) {
                            background_total[component] += effect_source_[
                                sample_pixel + component];
                        }
                        ++background_count;
                        continue;
                    }
                    for (std::size_t component = 0U; component < 3U; ++component) {
                        model_total[component] += effect_source_[
                            sample_pixel + component];
                    }
                    ++model_count;
                    if (centre != nullptr && surface != centre) {
                        const auto normal_dot = centre->normal_x * surface->normal_x
                            + centre->normal_y * surface->normal_y
                            + centre->normal_z * surface->normal_z;
                        if (normal_dot < 0.90F) {
                            for (std::size_t component = 0U;
                                 component < 3U; ++component) {
                                crease_total[component] += effect_source_[
                                    sample_pixel + component];
                            }
                            ++crease_count;
                        }
                    }
                }

                if (centre != nullptr && background_count != 0U) {
                    // Reconstruct a narrow fractional-coverage edge on both
                    // sides of the binary source silhouette. Keep most of the
                    // owning face at boundary pixels so the low-resolution
                    // models become clean rather than soft or out of focus.
                    const auto coverage = std::clamp(
                        (model_count + 5U) * 256U / 14U, 184U, 246U);
                    for (std::size_t component = 0U; component < 3U; ++component) {
                        const auto model_colour = (model_total[component]
                            + model_count / 2U) / model_count;
                        const auto background_colour = (background_total[component]
                            + background_count / 2U) / background_count;
                        rgba_[pixel + component] = static_cast<std::uint8_t>(
                            (model_colour * coverage
                                + background_colour * (256U - coverage) + 128U)
                            / 256U);
                    }
                } else if (centre == nullptr && model_count != 0U) {
                    const auto coverage = std::min(51U, model_count * 9U);
                    const auto background_divisor = std::max(1U, background_count);
                    for (std::size_t component = 0U; component < 3U; ++component) {
                        const auto model_colour = (model_total[component]
                            + model_count / 2U) / model_count;
                        const auto background_colour = (background_total[component]
                            + background_divisor / 2U) / background_divisor;
                        rgba_[pixel + component] = static_cast<std::uint8_t>(
                            (background_colour * (256U - coverage)
                                + model_colour * coverage + 128U) / 256U);
                    }
                } else if (crease_count != 0U) {
                    // Internal polygon boundaries receive a much narrower
                    // sub-pixel blend than the silhouette, retaining the
                    // low-poly facets while removing diagonal stair-steps.
                    const auto amount = std::min(38U, crease_count * 9U);
                    const auto source_pixel = effect_source_pixel(x, y);
                    for (std::size_t component = 0U; component < 3U; ++component) {
                        const auto crease_colour = (crease_total[component]
                            + crease_count / 2U) / crease_count;
                        rgba_[pixel + component] = static_cast<std::uint8_t>(
                            (effect_source_[source_pixel + component]
                                    * (256U - amount)
                                + crease_colour * amount + 128U) / 256U);
                    }
                }
            }
        }
    }

    void apply_rtx_lighting(
        const starfox::render::Framebuffer& framebuffer,
        const PresentationEffects& effects) {
        if (effects.model_surfaces == nullptr
            || effects.model_surfaces->empty()
            || framebuffer.stored_width() < 3U
            || framebuffer.stored_height() < 3U) {
            return;
        }
        const auto width = static_cast<std::size_t>(framebuffer.stored_width());
        const auto first_x = std::max(1, effects.model_surface_x
            + static_cast<std::int32_t>(effects.model_surfaces->minimum_x()));
        const auto first_y = std::max(1, effects.model_surface_y
            + static_cast<std::int32_t>(effects.model_surfaces->minimum_y()));
        const auto last_x = std::min(
            static_cast<std::int32_t>(framebuffer.stored_width()) - 1,
            effects.model_surface_x
                + static_cast<std::int32_t>(effects.model_surfaces->maximum_x()));
        const auto last_y = std::min(
            static_cast<std::int32_t>(framebuffer.stored_height()) - 1,
            effects.model_surface_y
                + static_cast<std::int32_t>(effects.model_surfaces->maximum_y()));
        constexpr std::array<float, 4> strengths{0.0F, 0.35F, 0.65F, 1.0F};
        const auto strength = strengths[rtx_lighting_];
        // Camera-space key light from above-left, neutral frontal fill, and
        // a camera-facing half vector for a broad, restrained highlight.
        constexpr std::array<float, 3> key{-0.474F, -0.632F, -0.613F};
        constexpr std::array<float, 3> fill{0.422F, 0.211F, -0.881F};
        constexpr std::array<float, 3> half_vector{-0.267F, -0.356F, -0.895F};
        // Row independent: neighbours come from the surface buffer and each pixel writes only itself.
        presentation_workers_.parallel_rows(
            static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, last_y - first_y)),
            [&](std::uint32_t slice_first, std::uint32_t slice_last) {
        for (auto y = first_y + static_cast<std::int32_t>(slice_first);
             y < first_y + static_cast<std::int32_t>(slice_last); ++y) {
            for (auto x = first_x; x < last_x; ++x) {
                const auto* sample = model_surface_at(framebuffer, effects, x, y);
                if (sample == nullptr) continue;
                auto nx = sample->normal_x;
                auto ny = sample->normal_y;
                auto nz = sample->normal_z;
                // Source shapes are not consistent about winding. Orient the
                // visible flat toward the camera before evaluating PC lights.
                if (nz > 0.0) {
                    nx = -nx;
                    ny = -ny;
                    nz = -nz;
                }
                const auto key_light = std::max(
                    0.0F, nx * key[0] + ny * key[1] + nz * key[2]);
                const auto fill_light = std::max(
                    0.0F, nx * fill[0] + ny * fill[1] + nz * fill[2]);
                const auto facing = std::clamp(-nz, 0.0F, 1.0F);
                const auto rim_base = 1.0F - facing;
                const auto rim = rim_base * rim_base * key_light * 0.10F;
                const auto specular_dot = std::max(0.0F,
                    nx * half_vector[0] + ny * half_vector[1]
                        + nz * half_vector[2]);
                const auto specular_2 = specular_dot * specular_dot;
                const auto specular_4 = specular_2 * specular_2;
                const auto specular_8 = specular_4 * specular_4;
                const auto specular = specular_8 * 0.18F;

                auto nearer_neighbours = 0U;
                constexpr std::array<std::array<std::int32_t, 2>, 4> adjacent{{
                    {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
                }};
                for (const auto& offset : adjacent) {
                    const auto* neighbour = model_surface_at(
                        framebuffer, effects,
                        x + offset[0] * static_cast<std::int32_t>(framebuffer.draw_scale()),
                        y + offset[1] * static_cast<std::int32_t>(framebuffer.draw_scale()));
                    if (neighbour != nullptr
                        && neighbour->depth < sample->depth
                            - std::max(20.0F, std::abs(sample->depth) * 0.02F)) {
                        ++nearer_neighbours;
                    }
                }
                const auto occlusion = static_cast<float>(nearer_neighbours) * 0.025F;
                const auto illumination = std::clamp(
                    0.62F + key_light * 0.58F + fill_light * 0.16F
                        + rim - occlusion,
                    0.56F, 1.30F);
                const auto pixel = (static_cast<std::size_t>(y) * width
                    + static_cast<std::size_t>(x)) * 4U;
                // Palette faces already contain shading. Keep diffuse light neutral
                // and roll highlights into the available headroom instead of clipping
                // channels or adding a glow to black pixels during source fades.
                const auto peak = static_cast<float>(std::max({
                    rgba_[pixel], rgba_[pixel + 1U], rgba_[pixel + 2U]}));
                if (peak == 0.0F) continue;
                const auto diffuse = illumination <= 1.0F ? illumination
                    : 1.0F + (illumination - 1.0F) * (1.0F - peak / 255.0F);
                const auto shine = (255.0F - peak * diffuse) * specular
                    * (peak / 255.0F);
                for (std::size_t component = 0U; component < 3U; ++component) {
                    const auto original = static_cast<float>(rgba_[pixel + component]);
                    const auto lit = original * diffuse
                        + shine * (0.20F + 0.80F * original / peak);
                    const auto value = original + (lit - original) * strength;
                    rgba_[pixel + component] = static_cast<std::uint8_t>(
                        std::clamp(static_cast<std::int32_t>(value + 0.5F),
                            0, 255));
                }
            }
        }
            });
    }

    void apply_fxaa(starfox::simulation::AntiAliasingMode mode) {
        if (texture_width_ < 3U || texture_height_ < 3U) return;
        auto threshold_floor = std::uint16_t{12U};
        auto relative_divisor = std::uint16_t{8U};
        auto centre_weight = std::uint32_t{2U};
        auto neighbour_weight = std::uint32_t{1U};
        switch (mode) {
        case starfox::simulation::AntiAliasingMode::light:
            threshold_floor = 20U;
            relative_divisor = 6U;
            centre_weight = 6U;
            break;
        case starfox::simulation::AntiAliasingMode::heavy:
            threshold_floor = 6U;
            relative_divisor = 12U;
            centre_weight = 1U;
            break;
        case starfox::simulation::AntiAliasingMode::medium:
        case starfox::simulation::AntiAliasingMode::off:
        default:
            break;
        }
        const auto total_weight = centre_weight + neighbour_weight * 2U;
        capture_effect_source_region(
            0, 0, static_cast<std::int32_t>(texture_width_),
            static_cast<std::int32_t>(texture_height_));
        const auto& source = effect_source_;
        const auto width = static_cast<std::size_t>(texture_width_);
        const auto pixel_count = width * texture_height_;
        luma_scratch_.resize(pixel_count);
        presentation_workers_.parallel_rows(texture_height_,
            [&](std::uint32_t first_row, std::uint32_t last_row) {
                for (auto y = first_row; y < last_row; ++y) {
                    const auto row = static_cast<std::size_t>(y) * width;
                    for (std::size_t x = 0; x < width; ++x) {
                        luma_scratch_[row + x] = luma(source, (row + x) * 4U);
                    }
                }
            });
        for (std::size_t y = 1U; y + 1U < texture_height_; ++y) {
            for (std::size_t x = 1U; x + 1U < texture_width_; ++x) {
                const auto pixel = (y * width + x) * 4U;
                const auto left = pixel - 4U;
                const auto right = pixel + 4U;
                const auto up = pixel - width * 4U;
                const auto down = pixel + width * 4U;
                const auto luma_pixel = y * width + x;
                const auto centre_luma = luma_scratch_[luma_pixel];
                const auto left_luma = luma_scratch_[luma_pixel - 1U];
                const auto right_luma = luma_scratch_[luma_pixel + 1U];
                const auto up_luma = luma_scratch_[luma_pixel - width];
                const auto down_luma = luma_scratch_[luma_pixel + width];
                const auto minimum = std::min({centre_luma, left_luma,
                    right_luma, up_luma, down_luma});
                const auto maximum = std::max({centre_luma, left_luma,
                    right_luma, up_luma, down_luma});
                const auto range = static_cast<std::uint16_t>(maximum - minimum);
                if (range < std::max<std::uint16_t>(
                        threshold_floor, maximum / relative_divisor)) continue;
                const auto horizontal = std::abs(
                    static_cast<std::int32_t>(left_luma)
                    - static_cast<std::int32_t>(right_luma));
                const auto vertical = std::abs(
                    static_cast<std::int32_t>(up_luma)
                    - static_cast<std::int32_t>(down_luma));
                const auto first = horizontal >= vertical ? up : left;
                const auto second = horizontal >= vertical ? down : right;
                for (std::size_t component = 0U; component < 3U; ++component) {
                    rgba_[pixel + component] = static_cast<std::uint8_t>(
                        (static_cast<std::uint32_t>(source[pixel + component])
                                * centre_weight
                            + static_cast<std::uint32_t>(
                                source[first + component]) * neighbour_weight
                            + static_cast<std::uint32_t>(
                                source[second + component]) * neighbour_weight)
                            / total_weight);
                }
            }
        }
    }

    void set_windowed_size(std::uint32_t width, std::uint32_t height) noexcept {
        const auto raster_width = width / window_scale_;
        const auto raster_height = height / window_scale_;
        const auto integer_scale = raster_width <= snes_width
            ? 4U : (raster_width <= widescreen_16_9_width ? 3U : 2U);
        SDL_SetWindowSize(window_,
            static_cast<int>(starfox::render::presentation_width(
                raster_width * integer_scale,raster_height * integer_scale)),
            static_cast<int>(raster_height * integer_scale));
    }

    void ensure_bloom_texture(std::uint32_t width,std::uint32_t height) {
        if(bloom_texture_) return;
        bloom_texture_=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,static_cast<int>(width),static_cast<int>(height));
        if(!bloom_texture_ || !SDL_SetTextureScaleMode(bloom_texture_,SDL_SCALEMODE_LINEAR)
            || !SDL_SetTextureBlendMode(bloom_texture_,SDL_BLENDMODE_ADD))
            throw std::runtime_error{std::string{"SDL bloom texture: "}+SDL_GetError()};
    }
    void present_rgba_pixels(std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> rgba, bool gpu_uploaded=false) {
        if(stereo_display_active_) {
            if(!SDL_SetRenderLogicalPresentation(renderer_,
                int(starfox::render::presentation_width(width,height)),int(height),
                SDL_LOGICAL_PRESENTATION_LETTERBOX)) throw std::runtime_error(SDL_GetError());
            stereo_display_active_=false;
        }
        update_temporary_status();
        const auto base = smooth_layer_ready_
            ? std::span<const std::uint8_t>{smooth_base_rgba_}
            : bloom_layer_ready_ ? std::span<const std::uint8_t>{bloom_base_rgba_} : rgba;
        if (!gpu_uploaded && !SDL_UpdateTexture(texture_, nullptr, base.data(),
                static_cast<int>(width * 4U))) {
            throw std::runtime_error{std::string{"SDL_UpdateTexture: "} + SDL_GetError()};
        }
        if (smooth_layer_ready_) {
            ensure_1440p_model_textures(width, height);
            if (!gpu_uploaded && !SDL_UpdateTexture(smooth_model_texture_, nullptr,
                    smooth_model_rgba_.data(), static_cast<int>(width * 4U))) {
                throw std::runtime_error{
                    std::string{"SDL_UpdateTexture (1440p model): "}
                    + SDL_GetError()};
            }
            if (!SDL_SetRenderTarget(renderer_, smooth_target_texture_)) {
                throw std::runtime_error{
                    std::string{"SDL_SetRenderTarget (1440p model): "}
                    + SDL_GetError()};
            }
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
            SDL_RenderClear(renderer_);
            SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
            SDL_RenderTexture(renderer_, smooth_model_texture_, nullptr, nullptr);
            if (!SDL_SetRenderTarget(renderer_, nullptr)) {
                throw std::runtime_error{
                    std::string{"SDL_SetRenderTarget (window): "}
                    + SDL_GetError()};
            }
        }
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, smooth_layer_ready_
            ? smooth_target_texture_ : texture_, nullptr, nullptr);
        if (bloom_layer_ready_) {
            ensure_bloom_texture(width,height);
            if (!gpu_uploaded && !SDL_UpdateTexture(bloom_texture_, nullptr, bloom_glow_rgba_.data(),
                    static_cast<int>(width * 4U))) {
                throw std::runtime_error{std::string{"SDL bloom upload: "} + SDL_GetError()};
            }
            SDL_RenderTexture(renderer_, bloom_texture_, nullptr, nullptr);
        }
        if(const auto* capture=std::getenv("STARFOX_CAPTURE_PRESENTATION_PATH")) {
            if(const auto* frames=std::getenv("STARFOX_TEST_FRAMES")) {
                const bool final_capture=++presentation_capture_frames_==std::stoull(frames);
                const bool capture_sequence=std::getenv("STARFOX_CAPTURE_PRESENTATION_SEQUENCE")!=nullptr;
                if(final_capture || capture_sequence) {
                    // Hidden windows may suppress their backbuffer rendering.
                    // Render the same final layers to an explicit target with
                    // the corrected presentation dimensions for reliable QA.
                    auto* target=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_TARGET,
                        static_cast<int>(starfox::render::presentation_width(width,height)),
                        static_cast<int>(height));
                    if(!target) throw std::runtime_error(SDL_GetError());
                    if(!SDL_SetRenderTarget(renderer_,target)) {
                        SDL_DestroyTexture(target);throw std::runtime_error(SDL_GetError());
                    }
                    SDL_SetRenderDrawColor(renderer_,0,0,0,255);
                    SDL_RenderClear(renderer_);
                    SDL_RenderTexture(renderer_,smooth_layer_ready_?smooth_target_texture_:texture_,nullptr,nullptr);
                    if(bloom_layer_ready_) SDL_RenderTexture(renderer_,bloom_texture_,nullptr,nullptr);
                    auto* surface=SDL_RenderReadPixels(renderer_,nullptr);
                    SDL_SetRenderTarget(renderer_,nullptr);
                    SDL_DestroyTexture(target);
                    if(!surface) throw std::runtime_error(SDL_GetError());
                    bool saved=true;
                    if(capture_sequence) {
                        const auto path=std::string(capture)+".frame-"+std::to_string(presentation_capture_frames_)+".bmp";
                        saved=SDL_SaveBMP(surface,path.c_str());
                    }
                    if(final_capture) saved=SDL_SaveBMP(surface,capture) && saved;
                    SDL_DestroySurface(surface);
                    if(!saved) throw std::runtime_error(SDL_GetError());
                }
            }
        }
        last_present_succeeded_=SDL_RenderPresent(renderer_);
    }
    void ensure_dimensions(std::uint32_t width, std::uint32_t height) {
        if (width == texture_width_ && height == texture_height_) return;
        SDL_DestroyTexture(bloom_texture_);
        bloom_texture_ = nullptr;
        SDL_DestroyTexture(texture_);
        SDL_DestroyTexture(smooth_model_texture_);
        SDL_DestroyTexture(smooth_target_texture_);
        smooth_model_texture_ = nullptr;
        smooth_target_texture_ = nullptr;
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(width), static_cast<int>(height));
        if (texture_ == nullptr) {
            throw std::runtime_error{
                std::string{"SDL_CreateTexture: "} + SDL_GetError()};
        }
        SDL_SetTextureScaleMode(texture_, enhanced_graphics_
            ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        if (!SDL_SetRenderLogicalPresentation(renderer_,
                static_cast<int>(starfox::render::presentation_width(width,height)), static_cast<int>(height),
                SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
            throw std::runtime_error{
                std::string{"SDL_SetRenderLogicalPresentation: "}
                + SDL_GetError()};
        }
        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0U) {
            set_windowed_size(width, height);
        }
        texture_width_ = width;
        texture_height_ = height;
    }

    std::uint32_t window_scale_{1U};
    SDL_Window* window_{};
    DlssHost* dlss_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    SDL_Texture* bloom_texture_{};
    bool bloom_layer_ready_{};
    std::vector<std::uint8_t> bloom_base_rgba_, bloom_glow_rgba_;
    SDL_Texture* smooth_model_texture_{};
    SDL_Texture* smooth_target_texture_{};
    std::array<SDL_Texture*,2> stereo_eye_textures_{};
    SDL_Texture* stereo_packed_texture_{};
    unsigned stereo_packed_width_{};
    bool stereo_display_active_{};
    unsigned stereo_eye_width_{},stereo_eye_height_{};
    std::uint32_t texture_width_{snes_width};
    std::uint64_t presentation_capture_frames_{};
    std::uint64_t stereo_capture_frames_{};
    std::uint32_t texture_height_{snes_height};
    bool relative_mouse_mode_{};
    starfox::simulation::AntiAliasingMode anti_aliasing_{
        starfox::simulation::AntiAliasingMode::off};
    bool enhanced_graphics_{};
    bool smooth_polys_{};
    std::uint8_t rtx_lighting_{};
    starfox::render::TwoDFilter two_d_filter_{
        starfox::render::TwoDFilter::off};
    starfox::render::PixelFilterScratch pixel_filter_scratch_;
    // Shared by every presentation pass, not just the filter: they are all row
    // independent and all scale with the square of the render scale.
    starfox::render::RowWorkers presentation_workers_;
    std::vector<std::uint32_t> overlay_argb_;
    bool vsync_{};
    starfox::simulation::RendererMode renderer_mode_{
        starfox::simulation::RendererMode::gpu};
    bool smooth_layer_ready_{};
    std::uint32_t smooth_source_width_{};
    std::uint32_t smooth_source_height_{};
    std::uint32_t smooth_target_width_{};
    std::optional<std::chrono::steady_clock::time_point>
        temporary_status_until_;
    std::vector<std::uint8_t> rgba_;
    std::vector<std::uint8_t> chromatic_scratch_;
    std::vector<std::uint8_t> smooth_base_rgba_;
    std::vector<std::uint8_t> smooth_model_rgba_;
    std::vector<std::uint8_t> smooth_model_mask_;
    // Reused presentation scratch avoids allocating and copying a complete
    // 32:9 frame separately for every optional model effect.
    std::vector<std::uint8_t> effect_source_;
    std::vector<std::uint8_t> style_scratch_;
    starfox::render::BloomPass bloom_pass_;
    starfox::render::GpuEffects gpu_effects_;
    starfox::render::SdlGpuEffects sdl_gpu_effects_;
    starfox::render::GpuRaster native_raster_;
    starfox::render::GpuScene native_scene_;
    starfox::render::ModelMotionHistory temporal_history_;
    std::vector<starfox::render::GpuSceneDraw> temporal_draws_;
    starfox::render::ModelMotionHistory::Frame temporal_frame_{};
    std::uint64_t temporal_serial_{},temporal_epoch_{},temporal_scene_{};
    std::uint32_t temporal_context_{};
    bool temporal_enabled_{},temporal_pending_{};
    bool last_present_succeeded_{};
    starfox::render::GpuScene late_scene_;
    starfox::render::GpuScene background_scene_;
    starfox::render::GpuStereoScene native_stereo_scene_;
    bool stereo_scene_ready_{};
    const starfox::render::GpuSceneRecording* recorded_scene_{};
    bool scene_failure_reported_{},scene_success_reported_{};
    starfox::render::GpuComposite native_composite_;
    starfox::render::GpuComposite temporal_composite_;
    bool native_direct_reported_{};
    bool native_readback_reported_{};
    bool native_host_overlay_reported_{};
    bool native_confirmation_reported_{};
    bool native_setup_reported_{},native_touch_reported_{};
    bool native_planet_overlay_reported_{};
    bool native_model_split_reported_{};
    bool native_wipe_reported_{};
    bool native_circle_reported_{};
    bool native_background_fade_reported_{};
    bool native_colour_math_reported_{};
    bool native_window_mask_reported_{};
    bool native_wipe_fallback_reported_{};
    starfox::render::shadows::PortableShadows resident_shadows_;
    starfox::render::shadows::SdlDxrShadows native_dxr_shadows_;
    std::array<starfox::render::shadows::SdlDxrShadows,2> stereo_native_dxr_shadows_;
    bool native_shadow_selected_{};
    std::array<bool,2> stereo_native_shadow_selected_{};
    std::array<starfox::render::shadows::PortableShadows,2> stereo_resident_shadows_;
    bool portable_gpu_{},gpu_fallback_reported_{};
    bool native_gpu_binning_{};
    bool gpu_frame_pending_{};
    bool gpu_direct_reported_{};
    std::uint8_t bloom_{};
    std::uint8_t bloom_2d_{};
    std::uint8_t model_smoothing_{};
    std::vector<std::uint8_t> smoothing_scratch_;
    starfox::render::Effect effect_{};
    std::uint8_t effect_intensity_{100U};
    starfox::render::Effect world_effect_{};
    std::uint8_t world_effect_intensity_{100U};
    std::vector<std::uint16_t> luma_scratch_;
    std::int32_t effect_source_x_{};
    std::int32_t effect_source_y_{};
    std::size_t effect_source_width_{};
};

class AudioOutput {
public:
    explicit AudioOutput(const starfox::audio::Msu1Pack& pack)
        : pack_(&pack), msu1_([&pack](std::uint16_t track) {
              return pack.load_track(track);
          }) {
        constexpr SDL_AudioSpec spec{
            SDL_AUDIO_S16, 2,
            static_cast<int>(starfox::audio::Spc700Audio::sample_rate)};
        stream_ = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (stream_ == nullptr) {
            throw std::runtime_error{
                std::string{"SDL_OpenAudioDeviceStream: "} + SDL_GetError()};
        }
    }

    ~AudioOutput() { SDL_DestroyAudioStream(stream_); }
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    struct PreparedState {
        starfox::audio::Spc700Audio emulator;
        starfox::audio::Msu1Audio msu;
        std::uint8_t music{}, effects{};
        std::uint32_t phase{}, speed{};
    };
    std::vector<std::uint8_t> save_state() const {
        starfox::state::Writer out;
        out(emulator_.save_state(), msu1_.save_state(), music_volume_, sfx_volume_,
            fast_sample_phase_, previous_speed_multiplier_);
        return out.bytes();
    }
    PreparedState prepare_state(std::span<const std::uint8_t> bytes) const {
        PreparedState result;
        std::vector<std::uint8_t> spc, msu;
        starfox::state::Reader in{bytes};
        in(spc, msu, result.music, result.effects, result.phase, result.speed);
        in.finish();
        if (result.music > 100 || result.effects > 100 || result.speed == 0
            || result.speed > 20 || result.phase >= result.speed)
            throw std::runtime_error{"Invalid saved audio mixer state"};
        result.emulator.load_state(spc);
        result.msu = starfox::audio::Msu1Audio{[pack = pack_](std::uint16_t track) {
            return pack->load_track(track);
        }};
        result.msu.load_state(msu);
        return result;
    }
    void commit_state(PreparedState&& state) {
        // No emulator state changes until the old playback queue is discarded.
        if (!SDL_ClearAudioStream(stream_)) throw std::runtime_error{SDL_GetError()};
        emulator_ = std::move(state.emulator);
        msu1_ = std::move(state.msu);
        music_volume_ = state.music;
        sfx_volume_ = state.effects;
        fast_sample_phase_ = state.phase;
        previous_speed_multiplier_ = state.speed;
        fast_samples_.clear(); mixed_samples_.clear();
    }

    void start() {
        if (started_) return;
        // Keep the device paused throughout the desktop/window preroll. The
        // previous 100 ms prime was consumed during that 1.5-second pause in
        // game activity, so source audio began with an empty SDL queue and
        // underruns on the first scheduling wobble. A small, non-emulated
        // lead-in starts the real stream with more than one logic tick of headroom
        // without advancing the SPC ahead of cartridge state.
        constexpr std::size_t startup_frames =
            starfox::audio::Spc700Audio::sample_rate
                * starfox::app::realtime_audio_startup_ms / 1'000U;
        const std::array<std::int16_t, startup_frames * 2U> silence{};
        if (!SDL_PutAudioStreamData(stream_, silence.data(),
                static_cast<int>(silence.size() * sizeof(silence.front())))) {
            throw std::runtime_error{
                std::string{"SDL_PutAudioStreamData: "} + SDL_GetError()};
        }
        if (!SDL_ResumeAudioStreamDevice(stream_)) {
            throw std::runtime_error{
                std::string{"SDL_ResumeAudioStreamDevice: "} + SDL_GetError()};
        }
        started_ = true;
    }

    void set_paused(bool paused) {
        if (!started_ || paused == paused_) return;
        const auto succeeded = paused
            ? SDL_PauseAudioStreamDevice(stream_)
            : SDL_ResumeAudioStreamDevice(stream_);
        if (!succeeded) {
            throw std::runtime_error{
                std::string{paused ? "SDL_PauseAudioStreamDevice: "
                                   : "SDL_ResumeAudioStreamDevice: "}
                + SDL_GetError()};
        }
        paused_ = paused;
    }

    void set_msu1_enabled(bool enabled) noexcept {
        msu1_.set_enabled(enabled);
    }
    void set_volumes(std::uint8_t music, std::uint8_t effects) noexcept {
        music_volume_ = std::min<std::uint8_t>(music, 100U);
        sfx_volume_ = std::min<std::uint8_t>(effects, 100U);
    }
    void set_game_paused(bool paused) noexcept {
        msu1_.set_paused(paused);
    }
    [[nodiscard]] std::uint16_t msu1_track() const noexcept {
        return msu1_.selected_track();
    }
    [[nodiscard]] bool msu1_playing() const noexcept {
        return msu1_.playing();
    }

    [[nodiscard]] std::array<std::uint8_t, 4> queue_logic_tick(
        std::span<const starfox::simulation::ApuPortWrite> writes,
        std::span<const starfox::simulation::MsuRegisterWrite> msu_writes,
        std::uint32_t speed_multiplier, bool queue_output = true) {
        starfox::audio::render_mixed_tick(emulator_,msu1_,writes,msu_writes,
            music_volume_,sfx_volume_,mixed_samples_);
        auto& samples = mixed_samples_;
        if(std::getenv("STARFOX_TEST_FRAMES") && std::getenv("STARFOX_TEST_AUDIO_SIGNATURES")) {
            const auto bytes=std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(samples.data()),samples.size()*sizeof(samples[0])};
            const bool silent=std::all_of(samples.begin(),samples.end(),[](auto sample){return sample==0;});
            std::cerr<<"audio-frame: samples="<<samples.size()<<" crc="<<starfox::assets::crc32(bytes)
                <<" silent="<<silent<<" track="<<msu1_.selected_track()<<" msu-playing="<<msu1_.playing()<<'\n';
        }
        std::span<const std::int16_t> queued_samples{samples};
        speed_multiplier = std::max(1U, speed_multiplier);
        if (speed_multiplier != previous_speed_multiplier_) {
            // Do not play the preceding speed's queued audio after a change.
            if (!SDL_ClearAudioStream(stream_)) {
                throw std::runtime_error{
                    std::string{"SDL_ClearAudioStream: "} + SDL_GetError()};
            }
            fast_sample_phase_ = 0U;
            previous_speed_multiplier_ = speed_multiplier;
        }
        if (speed_multiplier > 1U) {
            // The SPC still advances through its complete 50 ms source tick,
            // but accelerated playback consumes it in 1/N of that time.
            // Select complete stereo frames on a continuous modulo-N phase;
            // carrying the remainder across ticks prevents 3x from dropping
            // twenty samples per second and becoming audibly choppy.
            fast_samples_.clear();
            const auto source_frames = samples.size() / 2U;
            fast_samples_.reserve(
                ((source_frames + speed_multiplier - 1U) / speed_multiplier)
                * 2U);
            for (std::size_t source_frame = 0U;
                 source_frame < source_frames; ++source_frame) {
                if ((fast_sample_phase_ + source_frame) % speed_multiplier
                    != 0U) continue;
                const auto source = source_frame * 2U;
                fast_samples_.push_back(samples[source]);
                fast_samples_.push_back(samples[source + 1U]);
            }
            fast_sample_phase_ = static_cast<std::uint32_t>(
                (fast_sample_phase_ + source_frames) % speed_multiplier);
            queued_samples = fast_samples_;
        }
        constexpr auto max_queued_frames =
            starfox::audio::Spc700Audio::sample_rate
                * starfox::app::realtime_audio_limit_ms / 1'000U;
        if (queue_output && !starfox::app::queue_realtime_audio(
                stream_, queued_samples, max_queued_frames)) {
            throw std::runtime_error{
                std::string{"Audio playback queue: "} + SDL_GetError()};
        }
        return emulator_.output_ports();
    }

    [[nodiscard]] std::array<std::uint8_t, 4> prime_upload_sequence(
        std::span<const starfox::simulation::ApuPortWrite> writes) {
        static_cast<void>(emulator_.prime_upload_sequence(writes));
        return emulator_.output_ports();
    }

private:
    const starfox::audio::Msu1Pack* pack_{};
    starfox::audio::Spc700Audio emulator_;
    starfox::audio::Msu1Audio msu1_;
    SDL_AudioStream* stream_{};
    std::vector<std::int16_t> fast_samples_;
    std::vector<std::int16_t> mixed_samples_;
    std::uint8_t music_volume_{100U};
    std::uint8_t sfx_volume_{100U};
    std::uint32_t fast_sample_phase_{};
    std::uint32_t previous_speed_multiplier_{1U};
    bool started_{};
    bool paused_{};
};

class RumbleOutput {
public:
    explicit RumbleOutput(const starfox::assets::SymbolMap& symbols)
        : command_(address(symbols, "RUMBLE_CMD")),
          time_(address(symbols, "RUMBLE_TIME")),
          index_(address(symbols, "RUMBLE_INDEX")),
          table_(address(symbols, "RUMBLE_TABLE")) {}

    void advance(starfox::simulation::MapVm& map, SDL_Gamepad* gamepad,
        bool enabled) noexcept {
        if (!available() || !enabled || gamepad == nullptr) {
            stop(gamepad);
            return;
        }
        auto output = std::uint8_t{};
        auto sequence_index = map.read_native_byte(index_);
        for (std::size_t guard = 0U; guard < 4U; ++guard) {
            if (sequence_index == 0U) {
                output = map.read_native_byte(time_) == 0U
                    ? 0U : map.read_native_byte(command_);
                break;
            }
            output = map.read_native_byte(
                table_ + static_cast<std::uint32_t>(sequence_index - 1U));
            sequence_index = static_cast<std::uint8_t>(sequence_index + 1U);
            map.write_native_byte(index_, sequence_index);
            if (output == 0x19U) {
                map.write_native_byte(index_, 0U);
                output = 0U;
                break;
            }
            if (output != 0x91U) break;
            sequence_index = 1U;
            map.write_native_byte(index_, sequence_index);
        }
        const auto remaining = map.read_native_byte(time_);
        if (remaining != 0U) {
            map.write_native_byte(time_,
                static_cast<std::uint8_t>(remaining - 1U));
        }
        const auto high_frequency = static_cast<std::uint16_t>(
            (output & 0x0fU) * 0x1111U);
        const auto low_frequency = static_cast<std::uint16_t>(
            ((output >> 4U) & 0x0fU) * 0x1111U);
        static_cast<void>(SDL_RumbleGamepad(
            gamepad, low_frequency, high_frequency, 40U));
        active_ = output != 0U;
    }

    void stop(SDL_Gamepad* gamepad) noexcept {
        if (!active_) return;
        if (gamepad != nullptr) {
            static_cast<void>(SDL_RumbleGamepad(gamepad, 0U, 0U, 0U));
        }
        active_ = false;
    }

private:
    static std::uint32_t address(
        const starfox::assets::SymbolMap& symbols, const char* name) noexcept {
        const auto found = symbols.find(name);
        return found.empty() ? 0U : found.front();
    }
    [[nodiscard]] bool available() const noexcept {
        return command_ != 0U && time_ != 0U && index_ != 0U && table_ != 0U;
    }

    std::uint32_t command_{};
    std::uint32_t time_{};
    std::uint32_t index_{};
    std::uint32_t table_{};
    bool active_{};
};

class MsuFadeOutput {
public:
    explicit MsuFadeOutput(const starfox::assets::SymbolMap& symbols)
        : flag_(address(symbols, "MSUFADEFLAG")),
          volume_(address(symbols, "CURMSUVOLUME")) {}

    [[nodiscard]] std::optional<starfox::simulation::MsuRegisterWrite>
        advance(starfox::simulation::MapVm& map) const noexcept {
        if (flag_ == 0U || volume_ == 0U
            || map.read_native_byte(flag_) == 0U) return std::nullopt;
        const auto current = map.read_native_byte(volume_);
        if (current == 0U) return std::nullopt;
        const auto next = static_cast<std::uint8_t>(
            current > 3U ? current - 3U : 0U);
        map.write_native_byte(volume_, next);
        return starfox::simulation::MsuRegisterWrite{0x2006U, next, 0U};
    }

private:
    static std::uint32_t address(
        const starfox::assets::SymbolMap& symbols, const char* name) noexcept {
        const auto found = symbols.find(name);
        return found.empty() ? 0U : found.front();
    }
    std::uint32_t flag_{};
    std::uint32_t volume_{};
};

class PresentationPacer {
public:
    void wait_for_next_frame(
        std::uint32_t presentation_hz = starfox::timing::kPresentationHz) {
        if (presentation_hz == 0U) {
            throw std::invalid_argument{"presentation FPS cannot be zero"};
        }
        auto now = std::chrono::steady_clock::now();
        if (presentation_hz != presentation_hz_) {
            epoch_ = now;
            frame_ = 0U;
            presentation_hz_ = presentation_hz;
        }
        ++frame_;
        auto deadline = epoch_ + std::chrono::nanoseconds{
            static_cast<std::chrono::nanoseconds::rep>(
                frame_ * 1'000'000'000ULL / presentation_hz_)};
        now = std::chrono::steady_clock::now();
        if (now < deadline) {
            std::this_thread::sleep_until(deadline);
            return;
        }
        // Do not emit a burst of catch-up presentations after a debugger stop
        // or suspended laptop; the simulation clock already clamps that gap.
        if (now - deadline > std::chrono::milliseconds{250}) {
            epoch_ = now;
            frame_ = 0;
        }
    }

private:
    std::chrono::steady_clock::time_point epoch_{std::chrono::steady_clock::now()};
    std::uint64_t frame_{};
    std::uint32_t presentation_hz_{starfox::timing::kPresentationHz};
};

struct RemapMenuState {
    bool active{};
    bool waiting_for_input{};
    starfox::app::BindingDevice device{
        starfox::app::BindingDevice::gamepad};
    std::size_t action{};
};

struct HudEditorState {
    bool active{};
    std::optional<starfox::render::HudElement> dragging;
    float pointer_x{-1.0F};
    float pointer_y{-1.0F};
    float grab_x{};
    float grab_y{};
};

void draw_hud_editor_chrome(
    starfox::render::Framebuffer& framebuffer,
    const starfox::render::ScaledTextRenderer& text_renderer,
    const HudEditorState& editor,
    const starfox::render::HudLayout& layout,
    starfox::simulation::Experience experience,
    starfox::simulation::DisplayMode display_mode,
    std::uint8_t background_colour,
    std::uint8_t foreground_colour) {
    const auto width = framebuffer.width();
    const auto solid = [&framebuffer](
                           std::int32_t x, std::int32_t y,
                           std::int32_t box_width, std::int32_t box_height,
                           std::uint8_t colour) {
        for (std::int32_t row = 0; row < box_height; ++row) {
            for (std::int32_t column = 0; column < box_width; ++column) {
                framebuffer.set(x + column, y + row, colour);
            }
        }
    };
    const auto box = [&solid](HudRect rect, std::uint8_t colour) {
        solid(rect.x, rect.y, rect.width, 1, colour);
        solid(rect.x, rect.y + rect.height - 1, rect.width, 1, colour);
        solid(rect.x, rect.y, 1, rect.height, colour);
        solid(rect.x + rect.width - 1, rect.y, 1, rect.height, colour);
    };
    solid(0, 0, static_cast<std::int32_t>(width), 11,
        background_colour);
    const auto title_label = (experience
            == starfox::simulation::Experience::starfox_ex
            ? std::string_view{"EX HUD"}
            : (width == snes_width ? std::string_view{"HUD"}
                                   : std::string_view{"HUD LAYOUT"}));
    const auto profile = display_profile_name(display_mode);
    const auto label_width = text_renderer.measure_ascii(title_label);
    const auto title_width = label_width + 8 + text_renderer.measure_ascii(profile);
    const auto title_x = std::max<std::int32_t>(0,
        (static_cast<std::int32_t>(width) - title_width) / 2);
    text_renderer.draw_ascii(title_label, title_x,
        2, framebuffer, 0U, foreground_colour);
    text_renderer.draw_ascii(profile, title_x + label_width + 8,
        2, framebuffer, 0U, foreground_colour);
    solid(0, 210, static_cast<std::int32_t>(width), 14,
        background_colour);
    const auto reset = hud_reset_button_rect(width);
    const auto done = hud_done_button_rect(width);
    if (reset.contains(editor.pointer_x, editor.pointer_y)) {
        box(reset, foreground_colour);
    }
    if (done.contains(editor.pointer_x, editor.pointer_y)) {
        box(done, foreground_colour);
    }
    text_renderer.draw_ascii("Y RESET", reset.x + (reset.width - text_renderer.measure_ascii("Y RESET")) / 2,
        reset.y + 1, framebuffer, 0U, foreground_colour);
    text_renderer.draw_ascii("B DONE", done.x + (done.width - text_renderer.measure_ascii("B DONE")) / 2,
        done.y + 1, framebuffer, 0U, foreground_colour);

    std::optional<starfox::render::HudElement> hovered;
    auto hovered_area = std::numeric_limits<std::int32_t>::max();
    for (std::uint8_t value = 0U;
         value < static_cast<std::uint8_t>(starfox::render::HudElement::count);
         ++value) {
        const auto element =
            static_cast<starfox::render::HudElement>(value);
        const auto rect = placed_hud_rect(
            element, width, layout, experience);
        const auto area = rect.width * rect.height;
        if (rect.contains(editor.pointer_x, editor.pointer_y)
            && area < hovered_area) {
            hovered = element;
            hovered_area = area;
        }
    }
    const auto selected = editor.dragging ? editor.dragging : hovered;
    if (selected) {
        auto rect = placed_hud_rect(
            *selected, width, layout, experience);
        constexpr std::int32_t length = 5;
        --rect.x;
        --rect.y;
        rect.width += 2;
        rect.height += 2;
        solid(rect.x, rect.y, length, 1, foreground_colour);
        solid(rect.x, rect.y, 1, length, foreground_colour);
        solid(rect.x + rect.width - length, rect.y,
            length, 1, foreground_colour);
        solid(rect.x + rect.width - 1, rect.y,
            1, length, foreground_colour);
        solid(rect.x, rect.y + rect.height - 1,
            length, 1, foreground_colour);
        solid(rect.x, rect.y + rect.height - length,
            1, length, foreground_colour);
        solid(rect.x + rect.width - length,
            rect.y + rect.height - 1, length, 1, foreground_colour);
        solid(rect.x + rect.width - 1,
            rect.y + rect.height - length, 1, length,
            foreground_colour);
    }
}

struct CameraPoint {
    double x{};
    double y{};
    double z{};
};

struct MouseCameraState {
    bool active{};
    double pitch_offset{};
    double yaw_offset{};
    double zoom_offset{};
};

struct ExMouseInputLatch {
    std::int32_t delta_x{};
    std::int32_t delta_y{};
    std::uint8_t buttons{};
    std::int32_t scope_x{0x8a};
    std::int32_t scope_y{0x62};

    void add_motion(float x, float y) noexcept {
        const auto rounded_x = std::lround(x);
        const auto rounded_y = std::lround(y);
        delta_x = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(delta_x) + rounded_x,
            std::numeric_limits<std::int16_t>::min(),
            std::numeric_limits<std::int16_t>::max());
        delta_y = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(delta_y) + rounded_y,
            std::numeric_limits<std::int16_t>::min(),
            std::numeric_limits<std::int16_t>::max());
        scope_x = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(scope_x) + rounded_x, 0, 255);
        scope_y = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(scope_y) + rounded_y, 0, 223);
    }

    void set_button(std::uint8_t mask, bool held) noexcept {
        if (held) buttons = static_cast<std::uint8_t>(buttons | mask);
        else buttons = static_cast<std::uint8_t>(buttons & ~mask);
    }

    [[nodiscard]] starfox::simulation::MouseInputState consume() noexcept {
        const auto result = starfox::simulation::MouseInputState{
            static_cast<std::int16_t>(delta_x),
            static_cast<std::int16_t>(delta_y),
            buttons,
            static_cast<std::uint8_t>(scope_x),
            static_cast<std::uint8_t>(scope_y),
        };
        delta_x = 0;
        delta_y = 0;
        return result;
    }

    void release() noexcept {
        delta_x = 0;
        delta_y = 0;
        buttons = 0U;
    }
};

class TouchControls {
public:
    static constexpr bool enabled =
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS) \
    || defined(__IPHONEOS__)
        true;
#else
        false;
#endif

    TouchControls() noexcept : visible_{enabled} {}

    void update(SDL_FingerID finger, float x, float y) {
        if constexpr (!enabled) {
            static_cast<void>(finger);
            static_cast<void>(x);
            static_cast<void>(y);
            return;
        }
        visible_ = true;
        fingers_[finger] = hit_test(x, y);
    }
    void release(SDL_FingerID finger) noexcept { fingers_.erase(finger); }
    void reset() noexcept { fingers_.clear(); }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] ButtonMask buttons() const noexcept {
        auto result = ButtonMask{};
        for (const auto& [finger, buttons] : fingers_) {
            static_cast<void>(finger);
            result = static_cast<ButtonMask>(result | buttons);
        }
        return result;
    }

private:
    [[nodiscard]] static ButtonMask hit_test(float x, float y) noexcept {
        using namespace starfox::input;
        auto result = ButtonMask{};
        if (y < 0.18F) {
            if (x < 0.30F) result |= left_shoulder;
            if (x > 0.70F) result |= right_shoulder;
        }
        if (x >= 0.34F && x <= 0.47F && y >= 0.87F) {
            result |= starfox::input::select;
        }
        if (x >= 0.53F && x <= 0.66F && y >= 0.87F) result |= start;

        if (x < 0.43F && y > 0.43F) {
            constexpr float centre_x = 0.20F;
            constexpr float centre_y = 0.73F;
            const auto dx = x - centre_x;
            const auto dy = y - centre_y;
            if (dx < -0.045F) result |= left;
            if (dx > 0.045F) result |= right;
            if (dy < -0.045F) result |= up;
            if (dy > 0.045F) result |= down;
        }
        const auto inside = [x, y](float cx, float cy) {
            const auto dx = x - cx;
            const auto dy = y - cy;
            return dx * dx + dy * dy <= 0.0064F;
        };
        if (inside(0.89F, 0.69F)) result |= starfox::input::a;
        if (inside(0.77F, 0.81F)) result |= starfox::input::b;
        if (inside(0.77F, 0.57F)) result |= starfox::input::x;
        if (inside(0.65F, 0.69F)) result |= starfox::input::y;
        return result;
    }

    std::unordered_map<SDL_FingerID, ButtonMask> fingers_;
    bool visible_{};
};

std::uint16_t sample_ntt_data_pad(const bool* keys) noexcept {
    const auto shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    const auto digit = [keys, shift](SDL_Scancode primary, SDL_Scancode keypad) {
        return (!shift && keys[primary]) || keys[keypad];
    };
    std::uint16_t held{};
    if (digit(SDL_SCANCODE_0, SDL_SCANCODE_KP_0)) held |= 0x0001U;
    if (digit(SDL_SCANCODE_1, SDL_SCANCODE_KP_1)) held |= 0x0002U;
    if (digit(SDL_SCANCODE_2, SDL_SCANCODE_KP_2)) held |= 0x0004U;
    if (digit(SDL_SCANCODE_3, SDL_SCANCODE_KP_3)) held |= 0x0008U;
    if (digit(SDL_SCANCODE_4, SDL_SCANCODE_KP_4)) held |= 0x0010U;
    if (digit(SDL_SCANCODE_5, SDL_SCANCODE_KP_5)) held |= 0x0020U;
    if (digit(SDL_SCANCODE_6, SDL_SCANCODE_KP_6)) held |= 0x0040U;
    if (digit(SDL_SCANCODE_7, SDL_SCANCODE_KP_7)) held |= 0x0080U;
    if (digit(SDL_SCANCODE_8, SDL_SCANCODE_KP_8)) held |= 0x0100U;
    if (digit(SDL_SCANCODE_9, SDL_SCANCODE_KP_9)) held |= 0x0200U;
    if (keys[SDL_SCANCODE_KP_MULTIPLY]
        || (shift && keys[SDL_SCANCODE_8])) held |= 0x0400U;
    if (keys[SDL_SCANCODE_KP_DIVIDE]
        || (shift && keys[SDL_SCANCODE_3])) held |= 0x0800U;
    if (keys[SDL_SCANCODE_PERIOD] || keys[SDL_SCANCODE_KP_PERIOD]) {
        held |= 0x1000U;
    }
    if (keys[SDL_SCANCODE_C]) held |= 0x2000U;
    if (keys[SDL_SCANCODE_H]) held |= 0x8000U;
    return held;
}

double source_word_difference(double value, double origin) noexcept {
    auto difference = std::fmod(value - origin, 65'536.0);
    if (difference > 32'767.0) difference -= 65'536.0;
    else if (difference < -32'768.0) difference += 65'536.0;
    return difference;
}

std::int16_t interpolate_source_word(
    std::int16_t previous, std::int16_t current, double alpha) noexcept {
    alpha = std::clamp(alpha, 0.0, 1.0);
    const auto value = static_cast<std::int64_t>(std::lround(
        static_cast<double>(previous)
        + source_word_difference(current, previous) * alpha));
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}

starfox::simulation::CircleEffectState interpolate_circle_effect(
    const starfox::simulation::CircleEffectState& previous,
    const starfox::simulation::CircleEffectState& current,
    double alpha) noexcept {
    alpha = std::clamp(alpha, 0.0, 1.0);
    if (!previous.active && !current.active) return {};
    // Circle programs end on a source boundary. Holding the previous active
    // state for another interpolation interval leaves the death fill visibly
    // stuck after the cartridge has cleared CIRCLEANIM.
    if (previous.active && !current.active) return current;
    auto result = current;
    const auto start_radius = previous.active ? previous.radius : 0U;
    result.radius = static_cast<std::uint16_t>(std::lround(
        static_cast<double>(start_radius)
        + (static_cast<double>(current.radius) - start_radius) * alpha));
    if (previous.active) {
        result.centre_x = interpolate_source_word(
            previous.centre_x, current.centre_x, alpha);
        result.centre_y = interpolate_source_word(
            previous.centre_y, current.centre_y, alpha);
        const auto interpolate_component = [alpha](
            std::uint8_t from, std::uint8_t to) {
            return static_cast<std::uint8_t>(std::lround(
                static_cast<double>(from)
                + (static_cast<double>(to) - from) * alpha));
        };
        result.red = interpolate_component(previous.red, current.red);
        result.green = interpolate_component(previous.green, current.green);
        result.blue = interpolate_component(previous.blue, current.blue);
    }
    return result;
}

CameraPoint world_to_camera(
    double x, double y, double z,
    const starfox::timing::RenderTransform& camera,
    const starfox::simulation::MatrixQ15& matrix) {
    x = source_word_difference(x, camera.x);
    y = source_word_difference(y, camera.y);
    z = source_word_difference(z, camera.z);
    constexpr auto q15 = 32'768.0;
    return {
        (x * matrix[0] + y * matrix[3] + z * matrix[6]) / q15,
        (x * matrix[1] + y * matrix[4] + z * matrix[7]) / q15,
        (x * matrix[2] + y * matrix[5] + z * matrix[8]) / q15,
    };
}

// argv[0] is not necessarily an absolute or directly resolvable path when a
// Linux desktop launches the program through PATH or a shortcut.
std::filesystem::path executable_path(const char* argv0) {
#if defined(__SWITCH__)
    // libnx paths use a device prefix (sdmc:/). std::filesystem::absolute can
    // reinterpret that as a relative host path, so preserve argv[0] verbatim.
    if (argv0 != nullptr && *argv0 != '\0') {
        const auto launched = std::filesystem::path{argv0};
        if (launched.has_parent_path()) return launched;
        std::error_code error;
        const auto current = std::filesystem::current_path(error);
        if (!error && !current.empty()) return current / launched;
    }
    return std::filesystem::path{
        "sdmc:/switch/StarFoxEnhanced/StarFoxEnhanced.nro"};
#elif defined(SDL_PLATFORM_VITA)
    // Vita application files live under the title-id directory, while all
    // writable companions are deliberately redirected to ux0:data above.
    if (const auto* base_path = SDL_GetBasePath();
        base_path != nullptr && *base_path != '\0') {
        return std::filesystem::path{base_path} / "eboot.bin";
    }
    return std::filesystem::path{"ux0:app/SFXE00001/eboot.bin"};
#elif defined(STARFOX_UWP)
    if (const auto* base_path = SDL_GetBasePath();
        base_path != nullptr && *base_path != '\0') {
        return std::filesystem::path{base_path} / "starfox_pc.exe";
    }
    return {};
#elif defined(_WIN32)
    std::vector<wchar_t> path(32'768U);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) {
        throw std::runtime_error{"Cannot locate the running executable"};
    }
    return std::filesystem::path{std::wstring{path.data(), length}};
#elif defined(__APPLE__) && !defined(SDL_PLATFORM_IOS)
    std::uint32_t size{};
    static_cast<void>(_NSGetExecutablePath(nullptr, &size));
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) == 0) {
        return std::filesystem::weakly_canonical(path.data());
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    std::error_code error;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !resolved.empty()) return resolved;
#endif
    return std::filesystem::absolute(argv0);
}

} // namespace

#if defined(STARFOX_UWP)
extern "C" void starfox_uwp_log(const char* message) noexcept;
#endif
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS) || defined(STARFOX_UWP)
int SDL_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
#if defined(STARFOX_UWP)
    const auto log_uwp_startup = [](std::string_view message) noexcept {
        try { starfox_uwp_log(std::string{message}.c_str()); }
        catch (...) {}
    };
#endif
#if defined(_WIN32) && !defined(STARFOX_UWP)
    // Keep the lock alive through the catch block and its modal error dialog.
    // If it lived inside try, stack unwinding released it before MessageBoxA;
    // a second launch could then enter and display an identical second box.
    struct SingleInstanceMutex {
        HANDLE handle{};
        ~SingleInstanceMutex() {
            if (handle != nullptr) CloseHandle(handle);
        }
    } single_instance;
    if (std::getenv("STARFOX_TEST_FRAMES") == nullptr) {
        single_instance.handle = CreateMutexW(nullptr, FALSE,
            L"Local\\StarFoxEnhanced.NativePCRuntime.SingleInstance");
        if (single_instance.handle == nullptr) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    struct SingleInstanceLock {
        int descriptor{-1};
        ~SingleInstanceLock() {
            if (descriptor >= 0) ::close(descriptor);
        }
    } single_instance;
    if (std::getenv("STARFOX_TEST_FRAMES") == nullptr) {
        const auto lock_path = starfox::app::single_instance_lock_path();
        if (!lock_path.empty()) {
            std::error_code lock_error;
            std::filesystem::create_directories(
                lock_path.parent_path(), lock_error);
            single_instance.descriptor = ::open(
                lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (single_instance.descriptor >= 0
                && ::flock(single_instance.descriptor,
                    LOCK_EX | LOCK_NB) != 0) {
                return 0;
            }
        }
    }
#endif
    try {
        // UWP resolves SDL_GetPrefPath through WinRT LocalState.  Querying it
        // before SDL's platform bootstrap is complete can abort activation on
        // Xbox even though desktop Win32 happens to tolerate the same order.
        // Initialize every platform before the first settings/storage call.
        // DLSS initialization precedes DXGI. Reverse destruction keeps SDL's
        // graphics driver loaded until the SDK releases its retained device.
        std::optional<SdlContext> sdl;
        DlssHost dlss;
        sdl.emplace();
        const auto executable_directory = executable_path(argv[0]).parent_path();
        starfox::app::set_portable_data_directory(executable_directory);
        if (std::getenv("STARFOX_TEST_FRAMES") == nullptr) {
            starfox::app::migrate_legacy_user_data();
        }
#if defined(STARFOX_UWP)
        log_uwp_startup("SDL initialized");
#endif
        const auto saved_pregame_path = starfox::app::pregame_settings_path();
        auto saved_pregame = starfox::app::PregameSettings{};
        static_cast<void>(starfox::app::load_pregame_settings(
            saved_pregame_path, saved_pregame));
#if defined(STARFOX_UWP)
        log_uwp_startup("settings loaded");
#endif
        Window window{static_cast<starfox::simulation::RendererMode>(
            saved_pregame.renderer_mode),&dlss};
        dlss.bind(window.renderer());
        struct DlssShutdown {
            DlssHost& host;Window& window;
            ~DlssShutdown(){host.finish(window.renderer());}
        } dlss_shutdown{dlss,window};
#if defined(STARFOX_UWP)
        log_uwp_startup("window and renderer created");
#endif
        std::string initial_map = "BOOT";
        const starfox::audio::Msu1Pack msu1_pack{
            find_msu1_pack(executable_directory)};
#if defined(STARFOX_HAS_EMBEDDED_ASSETS)
        auto embedded_runtime_assets =
            load_or_compile_runtime_assets(executable_directory,
                window.renderer());
#if defined(STARFOX_UWP)
        log_uwp_startup("runtime assets loaded");
#endif
#endif
        const auto original_assets = [&]() -> RuntimeAssets {
            if (argc == 1 || argc == 2) {
                if (argc == 2) initial_map = argv[1];
#if defined(STARFOX_HAS_EMBEDDED_ASSETS)
                return std::move(embedded_runtime_assets.original);
#else
                std::filesystem::path rom_path;
                std::filesystem::path symbols_path;
                const auto workspace = executable_directory.parent_path().parent_path();
                const auto current = std::filesystem::current_path();
                const std::array candidates{
                    std::pair{executable_directory / "SF.SFC",
                        executable_directory / "SYMBOLS.TXT"},
                    std::pair{current / "SF.SFC", current / "SYMBOLS.TXT"},
                    std::pair{current / "upstream-ultrastarfox" / "SF.SFC",
                        current / "upstream-ultrastarfox" / "SYMBOLS.TXT"},
                    std::pair{workspace / "upstream-ultrastarfox" / "SF.SFC",
                        workspace / "upstream-ultrastarfox" / "SYMBOLS.TXT"},
                };
                for (const auto& [candidate_rom, candidate_symbols] : candidates) {
                    if (std::filesystem::exists(candidate_rom)
                        && std::filesystem::exists(candidate_symbols)) {
                        rom_path = candidate_rom;
                        symbols_path = candidate_symbols;
                        break;
                    }
                }
                if (rom_path.empty()) {
                    throw std::runtime_error{
                        "SF.SFC and SYMBOLS.TXT were not found beside the executable "
                        "or in upstream-ultrastarfox"};
                }
                return load_external_assets(rom_path, symbols_path);
#endif
            }
            if (argc == 3 || argc == 4) {
                if (argc == 4) initial_map = argv[3];
                return load_external_assets(argv[1], argv[2]);
            }
            std::cerr << "usage: starfox_pc [MAP]\n"
                         "   or: starfox_pc ROM SYMBOLS [MAP]\n";
            throw std::runtime_error{"invalid command-line arguments"};
        }();
        std::optional<RuntimeAssets> starfox_ex_assets;
#if defined(STARFOX_HAS_EMBEDDED_ASSETS)
        starfox_ex_assets.emplace(
            std::move(embedded_runtime_assets.starfox_ex));
#else
        const auto workspace = executable_directory.parent_path().parent_path();
        const auto current = std::filesystem::current_path();
        const std::array ex_candidates{
            std::pair{executable_directory / "SFES.SFC",
                executable_directory / "SFES-SYMBOLS.TXT"},
            std::pair{current / "upstream-star-fox-ex" / "SFES" / "SFES.SFC",
                current / "upstream-star-fox-ex" / "SYMBOLS.TXT"},
            std::pair{current / "build" / "upstream-star-fox-ex" / "SFES" / "SFES.SFC",
                current / "build" / "upstream-star-fox-ex" / "SYMBOLS.TXT"},
            std::pair{workspace / "upstream-star-fox-ex" / "SFES" / "SFES.SFC",
                workspace / "upstream-star-fox-ex" / "SYMBOLS.TXT"},
        };
        for (const auto& [candidate_rom, candidate_symbols] : ex_candidates) {
            if (std::filesystem::exists(candidate_rom)
                && std::filesystem::exists(candidate_symbols)) {
                starfox_ex_assets.emplace(
                    load_external_assets(candidate_rom, candidate_symbols));
                break;
            }
        }
#endif
        const auto ex_save_path = starfox::app::starfox_ex_save_ram_path();
        auto persisted_ex_save = std::vector<std::uint8_t>{};
        const auto persist_ex_save =
            std::getenv("STARFOX_TEST_FRAMES") == nullptr
            && std::getenv("STARFOX_TEST_EXPERIENCE") == nullptr
            && std::getenv("STARFOX_TEST_PRESSES") == nullptr;
        if (persist_ex_save) {
            static_cast<void>(starfox::app::load_starfox_ex_save_ram(
                ex_save_path, persisted_ex_save));
        }
        auto active_experience = static_cast<starfox::simulation::Experience>(
            saved_pregame.experience);
        if (const auto* forced_experience = std::getenv(
                "STARFOX_TEST_EXPERIENCE")) {
            active_experience = std::string_view{forced_experience} == "EX"
                ? starfox::simulation::Experience::starfox_ex
                : starfox::simulation::Experience::original;
        }
        const auto persist_pregame_changes =
            std::getenv("STARFOX_TEST_FRAMES") == nullptr
            && std::getenv("STARFOX_TEST_PRESSES") == nullptr
            && std::getenv("STARFOX_TEST_DISPLAY_MODE") == nullptr;
        bool restart_runtime = true;
        bool first_runtime = true;
        // Preview/experience changes rebuild the runtime, not the physical
        // input gesture. Keep held buttons latched until the user releases
        // them so an A press cannot activate another action after rebuilding.
        starfox::input::InputLatch input;
        bool launch_menu_preview = std::getenv("STARFOX_TEST_MENU_PREVIEW") != nullptr;
        bool launch_game_after_preview = false;
        std::optional<starfox::render::PresentationHistory>
            presentation_history;
        bool launch_hud_editor_preview =
            std::getenv("STARFOX_TEST_HUD_EDITOR") != nullptr;
        if (launch_hud_editor_preview || launch_menu_preview) {
            initial_map = "LEVEL1_1";
        }
        while (restart_runtime) {
        restart_runtime = false;
        const auto hud_editor_preview =
            std::exchange(launch_hud_editor_preview, false);
        const auto menu_preview = std::exchange(launch_menu_preview, false);
        if (active_experience == starfox::simulation::Experience::starfox_ex
            && !starfox_ex_assets) {
            // The selector persists its choice before requesting this restart,
            // so failing here would strand every later launch outside the
            // pre-game screen. Correct the stored choice and stay on Original.
            std::cerr << "starfox_pc: Star Fox EX runtime assets are not "
                         "installed in this build; using ORIGINAL\n";
            active_experience = starfox::simulation::Experience::original;
            saved_pregame.experience =
                static_cast<std::uint8_t>(active_experience);
            if (persist_pregame_changes) {
                static_cast<void>(starfox::app::save_pregame_settings(
                    saved_pregame_path, saved_pregame));
            }
        }
        const auto& assets = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? *starfox_ex_assets : original_assets;
        const auto& rom = assets.rom;
        const auto& symbols = assets.symbols;
        const starfox::assets::ShapeDecoder decoder{rom, symbols};
        const auto trigonometry = starfox::simulation::TrigTables::load(rom, symbols);
        const auto initial_ex_save = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? std::span<const std::uint8_t>{persisted_ex_save}
            : std::span<const std::uint8_t>{};
        starfox::simulation::GameSimulation game{
            rom, symbols, initial_map, initial_ex_save, true};
#if defined(STARFOX_UWP)
        log_uwp_startup("game simulation created");
#endif
        auto warned_ex_save_failure = false;
        const auto synchronize_ex_save = [&] {
            if (menu_preview || hud_editor_preview) return;
            if (active_experience
                    != starfox::simulation::Experience::starfox_ex) return;
            const auto current_save = game.ex_save_ram();
            if (persisted_ex_save.size() == current_save.size()
                && std::equal(persisted_ex_save.begin(),
                    persisted_ex_save.end(), current_save.begin())) return;
            if (persist_ex_save
                && !starfox::app::save_starfox_ex_save_ram(
                    ex_save_path, current_save)
                && !warned_ex_save_failure) {
                std::cerr << "warning: could not save Star Fox EX cartridge RAM to "
                          << ex_save_path << '\n';
                warned_ex_save_failure = true;
            }
            // Keep the last observed image even if the filesystem is
            // unavailable. This prevents a failed write from being retried
            // every 20 Hz logic tick and preserves the choices if the user
            // switches experiences again within this process.
            persisted_ex_save.assign(current_save.begin(), current_save.end());
        };
        if (!hud_editor_preview) synchronize_ex_save();
        const auto capture_pregame_settings = [&game] {
            return starfox::app::PregameSettings{
                static_cast<std::uint8_t>(game.timing_mode()),
                game.presentation_fps(),
                static_cast<std::uint8_t>(game.display_mode()),
                game.god_mode(),
                game.show_fps(),
                static_cast<std::uint8_t>(game.anti_aliasing_mode()),
                game.enhanced_graphics(),
                false,
                game.rtx_lighting_intensity(),
                static_cast<std::uint8_t>(game.two_d_filter()),
                game.vsync(),
                static_cast<std::uint8_t>(game.renderer_mode()),
                game.msu1_music(),
                game.rumble(),
                static_cast<std::uint8_t>(game.crosshair_colour()),
                static_cast<std::uint8_t>(game.experience()),
                game.music_volume(),
                game.sfx_volume(),
                static_cast<std::uint8_t>(game.render_scale()),
                game.on_screen_controls(),
                game.swap_face_buttons(),
                game.effect(),
                game.effect_intensity(),
                game.world_effect(),
                game.world_effect_intensity(),
                game.bloom(),
                game.bloom_2d(),
                game.model_smoothing(),
                game.language(),
                1U, // Reserved legacy settings slot; renderer owns line sizing.
                false, // Reserved legacy Enhanced Shadows setting.
                game.chromatic_aberration(),
                game.hdr_effect(),
                game.ray_tracing(),
                game.infinite_bombs(),
                game.infinite_boost(),
                game.default_laser(),
                game.selected_level(),
                game.stereo_output(),
                game.infinite_lives(),
            };
        };
        {
            game.set_timing_mode(static_cast<starfox::simulation::TimingMode>(
                saved_pregame.timing_mode));
            if (const auto* forced_timing = std::getenv(
                    "STARFOX_TEST_TIMING_MODE")) {
                game.set_timing_mode(std::string_view{forced_timing}
                        == "UNLOCKED"
                    ? starfox::simulation::TimingMode::unlocked_20_fps
                    : starfox::simulation::TimingMode::original_speed);
            }
            game.set_presentation_fps(saved_pregame.presentation_fps);
            if (const auto* forced_fps = std::getenv(
                    "STARFOX_TEST_PRESENTATION_FPS")) {
                game.set_presentation_fps(static_cast<std::uint16_t>(
                    std::stoul(forced_fps)));
            }
            game.set_display_mode(static_cast<starfox::simulation::DisplayMode>(
                saved_pregame.display_mode));
            if (const auto* forced_display = std::getenv(
                    "STARFOX_TEST_DISPLAY_MODE")) {
                game.set_display_mode(static_cast<
                    starfox::simulation::DisplayMode>(std::clamp(
                        std::stoi(forced_display), 0, 4)));
            }
            game.set_god_mode(saved_pregame.god_mode);
            game.set_show_fps(saved_pregame.show_fps);
            if(std::getenv("STARFOX_TEST_FRAMES")) {
                if(const auto* fps=std::getenv("STARFOX_TEST_SHOW_FPS"))
                    game.set_show_fps(std::string_view{fps}!="0");
            }
            game.set_anti_aliasing_mode(
                static_cast<starfox::simulation::AntiAliasingMode>(
                    saved_pregame.anti_aliasing));
            if (const auto* forced_aa = std::getenv(
                    "STARFOX_TEST_ANTI_ALIASING")) {
                const auto mode = std::string_view{forced_aa};
                game.set_anti_aliasing_mode(mode == "LIGHT" || mode == "1"
                        ? starfox::simulation::AntiAliasingMode::light
                    : mode == "MEDIUM" || mode == "2"
                        ? starfox::simulation::AntiAliasingMode::medium
                    : mode == "HEAVY" || mode == "3"
                        ? starfox::simulation::AntiAliasingMode::heavy
                        : starfox::simulation::AntiAliasingMode::off);
            }
            game.set_enhanced_graphics(false);
            // SMOOTH_POLYS is retained in the settings file only so older
            // revisions still load. Render Upscale replaces that effect.
            game.set_smooth_polys(false);
            game.set_rtx_lighting_intensity(saved_pregame.rtx_lighting);
            game.set_two_d_filter(
                saved_pregame.two_d_filter
                        < starfox::simulation::two_d_filter_mode_count
                    ? static_cast<starfox::simulation::TwoDFilterMode>(
                        saved_pregame.two_d_filter)
                    : starfox::simulation::TwoDFilterMode::off);
            game.set_vsync(saved_pregame.vsync);
            game.set_renderer_mode(
                static_cast<starfox::simulation::RendererMode>(
                    saved_pregame.renderer_mode));
            game.set_msu1_available(msu1_pack.available());
            game.set_msu1_music(saved_pregame.msu1_music);
            game.set_rumble(saved_pregame.rumble);
            game.set_music_volume(saved_pregame.music_volume);
            game.set_sfx_volume(saved_pregame.sfx_volume);
            game.set_on_screen_controls(saved_pregame.on_screen_controls);
            game.set_swap_face_buttons(saved_pregame.swap_face_buttons);
            if (const auto* forced_msu = std::getenv("STARFOX_TEST_MSU1")) {
                game.set_msu1_music(std::string_view{forced_msu} != "0");
            }
            if (const auto* forced_vsync = std::getenv("STARFOX_TEST_VSYNC")) {
                game.set_vsync(std::string_view{forced_vsync} != "0");
            }
            if (const auto* forced_renderer = std::getenv(
                    "STARFOX_TEST_RENDERER")) {
                const auto value = std::string_view{forced_renderer};
                game.set_renderer_mode(value == "SOFTWARE" || value == "1"
                    ? starfox::simulation::RendererMode::software
                    : starfox::simulation::RendererMode::gpu);
            }
            if (const auto* forced_enhanced = std::getenv(
                    "STARFOX_TEST_ENHANCED")) {
                game.set_two_d_filter(std::string_view{forced_enhanced} != "0"
                    ? starfox::simulation::TwoDFilterMode::edge
                    : starfox::simulation::TwoDFilterMode::off);
            }
            if (const auto* forced_lighting = std::getenv(
                    "STARFOX_TEST_RTX_LIGHTING")) {
                game.set_rtx_lighting(std::string_view{forced_lighting} != "0");
            }
            if (const auto* forced_filter = std::getenv(
                    "STARFOX_TEST_2D_FILTER")) {
                const auto value = std::string_view{forced_filter};
                game.set_two_d_filter(
                    value == "EDGE" || value == "1"
                        ? starfox::simulation::TwoDFilterMode::edge
                    : value == "SHARP" || value == "3"
                        ? starfox::simulation::TwoDFilterMode::sharp_bilinear
                    : value == "CRT" || value == "4"
                        ? starfox::simulation::TwoDFilterMode::crt
                    : value == "SCALEFX" || value == "5"
                        ? starfox::simulation::TwoDFilterMode::scalefx
                    : value == "XBRZ" || value == "2"
                        ? starfox::simulation::TwoDFilterMode::xbrz
                        : starfox::simulation::TwoDFilterMode::off);
            }
            game.set_crosshair_colour(
                static_cast<starfox::simulation::CrosshairColour>(
                    saved_pregame.crosshair_colour));
            if (std::getenv("STARFOX_TEST_FRAMES") != nullptr) {
                if (const auto* colour = std::getenv("STARFOX_TEST_CROSSHAIR_COLOUR")) {
                    game.set_crosshair_colour(static_cast<starfox::simulation::CrosshairColour>(
                        std::clamp(std::atoi(colour), 0, 7)));
                }
            }
            game.set_render_scale(static_cast<starfox::simulation::RenderScale>(
                saved_pregame.render_scale));
            if (const auto* forced_scale = std::getenv(
                    "STARFOX_TEST_RENDER_SCALE")) {
                const auto factor = std::atoi(forced_scale);
                if (factor >= 1 && factor <= static_cast<int>(
                        starfox::simulation::render_scale_count)) {
                    game.set_render_scale(
                        static_cast<starfox::simulation::RenderScale>(
                            factor - 1));
                }
            }
            game.set_experience(active_experience);
            game.set_effect(saved_pregame.effect);
            game.set_effect_intensity(saved_pregame.effect_intensity);
            game.set_world_effect(saved_pregame.world_effect);
            game.set_world_effect_intensity(saved_pregame.world_effect_intensity);
            game.set_bloom(saved_pregame.bloom);
            game.set_bloom_2d(saved_pregame.bloom_2d);
            game.set_model_smoothing(saved_pregame.model_smoothing);
            game.set_language(saved_pregame.language);
            if (const auto* language = std::getenv("STARFOX_TEST_LANGUAGE"))
                game.set_language(static_cast<std::uint8_t>(std::atoi(language)));
            game.set_ray_tracing(saved_pregame.ray_tracing);
            game.set_infinite_bombs(saved_pregame.infinite_bombs);
            game.set_infinite_lives(saved_pregame.infinite_lives);
            game.set_infinite_boost(saved_pregame.infinite_boost);
            game.set_default_laser(saved_pregame.default_laser);
            game.set_selected_level(saved_pregame.selected_level);
            game.set_stereo_output(saved_pregame.stereo_output);
            if(const auto* stereo=std::getenv("STARFOX_TEST_STEREO_OUTPUT"))
                game.set_stereo_output(static_cast<std::uint8_t>(std::atoi(stereo)));
            if (const auto* ray_tracing = std::getenv("STARFOX_TEST_RAY_TRACING"))
                game.set_ray_tracing(std::atoi(ray_tracing) != 0);
            game.set_chromatic_aberration(saved_pregame.chromatic_aberration);
            if (const auto* chromatic = std::getenv("STARFOX_TEST_CHROMATIC_ABERRATION"))
                game.set_chromatic_aberration(static_cast<std::uint8_t>(std::atoi(chromatic)));
            game.set_hdr_effect(saved_pregame.hdr_effect);
            if (const auto* hdr = std::getenv("STARFOX_TEST_HDR_EFFECT"))
                game.set_hdr_effect(static_cast<std::uint8_t>(std::atoi(hdr)));
            if (const auto* smoothing = std::getenv("STARFOX_TEST_MODEL_SMOOTHING"))
                game.set_model_smoothing(static_cast<std::uint8_t>(std::atoi(smoothing)));
            if (const auto* separated = std::getenv("STARFOX_TEST_SEPARATED_MODELS"))
                game.set_smooth_polys(std::atoi(separated) != 0);
            if (saved_pregame.effect == static_cast<std::uint8_t>(starfox::render::Effect::bloom)
                || saved_pregame.world_effect == static_cast<std::uint8_t>(starfox::render::Effect::bloom)) { game.set_bloom(2U); game.set_bloom_2d(2U); }
            if (const auto* bloom = std::getenv("STARFOX_TEST_BLOOM")) { game.set_bloom(static_cast<std::uint8_t>(std::atoi(bloom))); game.set_bloom_2d(static_cast<std::uint8_t>(std::atoi(bloom))); }
            if (const auto* forced_world = std::getenv("STARFOX_TEST_WORLD_EFFECT")) {
                game.set_world_effect(static_cast<std::uint8_t>(std::atoi(forced_world)));
            }
            if (const auto* forced_effect = std::getenv("STARFOX_TEST_EFFECT")) {
                game.set_effect(static_cast<std::uint8_t>(std::atoi(forced_effect)));
            }
            if (hud_editor_preview || menu_preview) {
                // Build the editor's static reference image from a genuine
                // cartridge-rendered Corneria frame. This hidden preroll stops
                // at the first stable gameplay chatter frame, so opening the
                // editor never exposes or continues the scramble sequence.
                game.set_god_mode(true);
                std::optional<std::uint32_t> first_meter_tick;
                std::uint32_t previous_dialogue_address{};
                std::uint8_t dialogue_count{};
                constexpr std::uint32_t maximum_preview_ticks = 2'400U;
                constexpr std::uint32_t meter_fallback_ticks = 720U;
                for (std::uint32_t tick = 0U;
                     tick < maximum_preview_ticks; ++tick) {
                    static_cast<void>(game.tick({}));
                    const auto meters = game.meter_state();
                    if (meters.enabled && !first_meter_tick) {
                        first_meter_tick = tick;
                    }
                    const auto dialogue = game.dialogue_state();
                    if (meters.enabled && dialogue.active
                        && dialogue.text_visible
                        && dialogue.text_address
                            != previous_dialogue_address) {
                        previous_dialogue_address = dialogue.text_address;
                        if (++dialogue_count >= 4U) {
                            // Let the formation finish crossing the viewport
                            // while retaining the same fourth chatter card.
                            // This is the clean, unobstructed static frame used
                            // by the original editor artwork.
                            constexpr std::uint8_t settle_ticks = 12U;
                            for (std::uint8_t settle = 0U;
                                 settle < settle_ticks; ++settle) {
                                static_cast<void>(game.tick({}));
                            }
                            break;
                        }
                    }
                    if (first_meter_tick
                        && tick - *first_meter_tick >= meter_fallback_ticks) {
                        break;
                    }
                }
            }
        }
        if (menu_preview) {
            game.set_god_mode(saved_pregame.god_mode);
            game.enable_menu_preview();
        }
        if (std::exchange(launch_game_after_preview, false)) {
            static_cast<void>(game.tick({starfox::input::start, starfox::input::start}));
        }
        // Explicit headless QA entry: run the real post-Andross continuation
        // with a populated route history. No production launch or saved game
        // is changed; both diagnostic switches must be present.
        if (std::getenv("STARFOX_TEST_FRAMES") != nullptr
            && std::getenv("STARFOX_TEST_ENDING") != nullptr) {
            const auto symbol = [&symbols](const char* name) {
                const auto& entries = symbols.find(name);
                if (entries.empty()) throw std::runtime_error{
                    std::string{"ending fixture missing symbol: "} + name};
                return entries.front();
            };
            constexpr std::array<unsigned, 6> scores{100, 90, 80, 70, 60, 100};
            constexpr std::array bosses{"BOSS11", "BOSS12", "BOSS13", "BOSS14", "BOSS15", "BOSSFINAL"};
            game.map().write_native_word(symbol("SPECPTR"), scores.size());
            for (std::size_t i = 0; i < scores.size(); ++i) {
                game.map().write_native_byte(symbol("SPECBUF") + i, scores[i]);
                game.map().write_native_word(symbol("BOSS_SEQ") + 2 * i,
                    symbol(bosses[i]) - symbol("ENDSEQBOSS"));
            }
            game.map().write_native_word(symbol("BOSS_PTR"), 2 * bosses.size());
            game.set_god_mode(true);
            game.start_map("FINALMAP_END");
            // Headless visual fixtures may advance the same source sequence
            // before capturing. This does not jump into a synthetic ending
            // state, and is unavailable without both test-only guards above.
            if (const auto* preroll = std::getenv("STARFOX_TEST_ENDING_PREROLL")) {
                const auto ticks = std::min(8'000ULL, std::stoull(preroll));
                for (std::uint64_t tick = 0; tick < ticks; ++tick) {
                    static_cast<void>(game.tick({}));
                }
            }
        }
        if (std::getenv("STARFOX_TEST_FRAMES") != nullptr) {
            if (std::getenv("STARFOX_TEST_NUCLEUS_DEFEAT") != nullptr)
                game.set_god_mode(true);
            if (const auto* preroll = std::getenv("STARFOX_TEST_PREROLL_TICKS")) {
                for (std::uint64_t tick = 0; tick < std::min(8'000ULL, std::stoull(preroll)); ++tick)
                    static_cast<void>(game.tick({}));
            }
            if (std::getenv("STARFOX_TEST_REVIVAL") != nullptr
                && std::getenv("STARFOX_TEST_REVIVAL_FRAME") == nullptr
                && game.objects().is_active(game.player())) {
                const auto restart_banks = symbols.find("MAPRESTARTBANK");
                const auto restart_positions = symbols.find("MAPRESTART");
                if (!restart_banks.empty() && !restart_positions.empty()
                    && game.map().read_native_byte(restart_banks.front()) == 0U
                    && game.map().read_native_word(restart_positions.front()) == 0U) {
                    throw std::runtime_error{"Forced revival requires an initialized map checkpoint; increase STARFOX_TEST_PREROLL_TICKS"};
                }
                for (const auto* name : {"MAPRESTARTBANK", "MAPRESTART", "RESTARTBG"}) {
                    const auto locations = symbols.find(name);
                    if (!locations.empty()) std::cerr << "revival-checkpoint " << name
                        << '=' << (std::string_view{name} == "MAPRESTARTBANK"
                            ? game.map().read_native_byte(locations.front())
                            : game.map().read_native_word(locations.front())) << '\n';
                }
                game.map().write_native_byte(symbols.find("LIVES").at(0), 2U);
                game.objects().at(game.player()).strategy_address =
                    symbols.find("PLAYERDEAD_ISTRAT").at(0);
            }
            if(const auto* message=std::getenv("STARFOX_TEST_MESSAGE")) {
                game.map().write_native_byte(symbols.find("FRIENDS_METER").at(0),
                    std::getenv("STARFOX_TEST_MESSAGE_METER")?255U:0U);
                starfox::simulation::Wdc65816Registers registers;
                registers.a=static_cast<std::uint16_t>(std::clamp(std::atoi(message),0,255));
                registers.status=0x24U;
                game.map().call_native_routine(symbols.find("SEND_MESSAGE_L").at(0),registers,2'000'000,true);
            }
            if(std::getenv("STARFOX_TEST_UPGRADE_FLASH") && game.objects().is_active(game.player())) {
                const auto flash=game.objects().allocate_after();
                if(!flash) throw std::runtime_error("No free slot for upgrade capture");
                auto& overlay=game.objects().at(flash);
                const auto& ship=game.objects().at(game.player());
                overlay.world_x=ship.world_x;overlay.world_y=ship.world_y;overlay.world_z=ship.world_z;
                overlay.shape=ship.shape;overlay.colour_table=ship.colour_table;
                overlay.rotation_x=ship.rotation_x;overlay.rotation_y=ship.rotation_y;overlay.rotation_z=ship.rotation_z;
                overlay.strategy_address=symbols.find("FLASHPLAYER_ISTRAT").at(0);
            }
            if(std::getenv("STARFOX_TEST_SCRAMBLE_WIPE")) {
                game.map().write_native_word(symbols.find("CIRCLEANIM").at(0),
                    static_cast<std::uint16_t>(symbols.find("MSCRAMWIPE_CIRCLE").at(0)));
            }
            if (const auto* style = std::getenv("STARFOX_TEST_EX_CROSSHAIR")) {
                const auto addresses = symbols.find("NOCROSSHAIRPLS");
                if (!addresses.empty()) game.map().write_native_byte(addresses.front(),
                    static_cast<std::uint8_t>(std::clamp(std::atoi(style),0,2)));
            }
            if (std::getenv("STARFOX_TEST_NUCLEUS_DEFEAT") != nullptr) {
                const auto shape = static_cast<std::uint16_t>(symbols.find("BOSS_8_0").at(0));
                bool found = false;
                for (const auto handle : game.objects().active_handles()) {
                    auto& object = game.objects().at(handle);
                    if (object.shape != shape) continue;
                    object.health = 0U;
                    object.strategy_flags[1] |= 0x01U;
                    object.strategy_address = symbols.find("BOSS8DIE_ISTRAT").at(0);
                    found = true;
                }
                if (!found) throw std::runtime_error{"nucleus defeat fixture has not reached the boss"};
            }
            if (std::getenv("STARFOX_TEST_TITANIA_END")) {
                // Enter at the authored setbg 2_3b command, retaining its
                // native background initializer and water scanline program.
                const auto entry = symbols.find("LEVEL2_3").at(0);
                const auto background = static_cast<std::uint16_t>(
                    symbols.find("BG_2_3B").at(0) - symbols.find("BGLISTS").at(0));
                auto limit = (entry & 0xff0000U) + 0xfffeU;
                for (const auto& [name, values] : symbols.entries()) {
                    if (!name.starts_with("LEVEL")) continue;
                    for (const auto value : values)
                        if (value > entry && value < limit) limit = value;
                }
                std::uint32_t command{};
                for (auto pc=entry; pc+2U<limit; ++pc)
                    if (rom.read8(pc)==16U && rom.read16(pc+1U)==background) { command=pc; break; }
                if (!command) throw std::runtime_error{"Titania background command not found"};
                game.set_god_mode(true);
                game.map().start(command, game.player());
                game.map().advance_distance(1);
                game.map().advance_distance(30000);
            }
            if (const auto* clear = std::getenv("STARFOX_TEST_CLEAR")) {
                const auto entry = symbols.find(initial_map).at(0);
                const auto target = symbols.find(clear).at(0);
                auto limit = std::min(entry + 16'384U, (entry & 0xff0000U) + 0xfffcU);
                for (const auto& [name, values] : symbols.entries()) {
                    if (!name.starts_with("LEVEL")) continue;
                    for (const auto value : values)
                        if (value > entry && value < limit) limit = value;
                }
                std::uint32_t call{};
                for (auto pc = entry; pc + 3U < limit; ++pc) {
                    if (rom.read8(pc) == 40 && (rom.read16(pc + 1) | 0x8000U) == (target & 0xffffU)
                        && rom.read8(pc + 3) == (target >> 16)) { call = pc; break; }
                }
                if (call == 0U) throw std::runtime_error{"clear fixture needs an authored level call"};
                game.set_god_mode(true);
                game.map().start(call, game.player());
                game.map().advance_distance(1);
            }
        }
        const auto save_pregame_settings = [&] {
            saved_pregame = capture_pregame_settings();
            if (persist_pregame_changes) {
                static_cast<void>(starfox::app::save_pregame_settings(
                    saved_pregame_path, saved_pregame));
            }
        };
        if (const auto* forced_display = std::getenv(
                "STARFOX_TEST_DISPLAY_MODE")) {
            const auto mode = std::string_view{forced_display};
            if (mode == "4_3") {
                game.set_display_mode(
                    starfox::simulation::DisplayMode::standard_4_3);
            } else if (mode == "16_9") {
                game.set_display_mode(
                    starfox::simulation::DisplayMode::widescreen_16_9);
            } else if (mode == "16_10") {
                game.set_display_mode(
                    starfox::simulation::DisplayMode::widescreen_16_10);
            } else if (mode == "21_9") {
                game.set_display_mode(
                    starfox::simulation::DisplayMode::ultrawide_21_9);
            } else if (mode == "32_9") {
                game.set_display_mode(
                    starfox::simulation::DisplayMode::super_ultrawide_32_9);
            }
        }
        const auto suppress_configurable_hud =
            std::getenv("STARFOX_TEST_HIDE_CONFIGURABLE_HUD") != nullptr;
        const auto ram_symbol = [&symbols](const char* name) {
            for (const auto address : symbols.find(name)) {
                if ((address >> 16U) == 0 || (address >> 16U) == 0x7eU) return address;
            }
            throw std::runtime_error{std::string{"missing runtime RAM symbol: "} + name};
        };
        const auto mario_symbol = [&symbols](const char* name) {
            for (const auto address : symbols.find(name)) {
                if ((address >> 16U) == 0x70U) return address;
            }
            throw std::runtime_error{
                std::string{"missing runtime Super FX symbol: "} + name};
        };
        const auto colour_symbol = [&symbols](const char* name) {
            for (const auto address : symbols.find(name)) {
                if ((address & 0xffffU) >= 0x8000U
                    && ((address >> 16U) & 0xffU) < 0x70U) {
                    return static_cast<std::uint16_t>(address);
                }
            }
            throw std::runtime_error{std::string{"missing colour symbol: "} + name};
        };
        const auto camera_x_address = ram_symbol("VIEWPOSX");
        const auto camera_y_address = ram_symbol("VIEWPOSY");
        const auto camera_float_y_address = ram_symbol("VIEWFLOATY");
        const auto camera_z_address = ram_symbol("VIEWPOSZ");
        const auto camera_pitch_address = ram_symbol("VIEWROTXW");
        const auto camera_yaw_address = ram_symbol("VIEWROTYW");
        const auto camera_roll_address = ram_symbol("VIEWROTZW");
        const auto game_frame_address = ram_symbol("GAMEFRAME");
        const auto background_x_address = ram_symbol("BG2XSCROLL");
        const auto background_y_address = ram_symbol("BG2SCROLL");
        const auto player_fly_mode_address = ram_symbol("PLAYERFLYMODE");
        const auto player_ship_flags_address = ram_symbol("PSHIPFLAGS");
        const auto hud_rotation_address = ram_symbol("HUDROT");
        const auto shadow_height_address = ram_symbol("SHADOWHEIGHT");
        const auto vanish_x_address = mario_symbol("M_VANISHX");
        const auto vanish_y_address = mario_symbol("M_VANISHY");
        const auto native_model_z_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_BIGZ") : 0U;
        const auto depth_colours_address = mario_symbol("M_DEPTHSTAB");
        const auto depth_thresholds_address = mario_symbol("M_DEPTHTABLE");
        const auto depth_table_addresses = symbols.find("DEPTHTABLES");
        if (depth_table_addresses.empty()) {
            throw std::runtime_error{"missing depth-table ROM symbol"};
        }
        const auto depth_table_address = depth_table_addresses.front();
        const auto hud_colour_address = mario_symbol("M_HUDCOLOUR");
        const auto hud_flags_address = mario_symbol("M_HUDFLAGS");
        const auto wire_mode_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_WIREMODE") : 0U;
        const auto wobble_mode_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_WOBBLEMODE") : 0U;
        const auto wave_mode_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_WABBLEMODE") : 0U;
        const auto cel_mode_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_CELMODE") : 0U;
        const auto wave_offset_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_SINEOFFSET") : 0U;
        const auto grid_lines_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_GRIDLINES") : 0U;
        const auto colour_warp_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_COLORWARP") : 0U;
        const auto projected_points_address = active_experience
                == starfox::simulation::Experience::starfox_ex
            ? mario_symbol("M_PROJPNTS") : 0U;
        const auto ex_title_intro_background = [&symbols]() {
            const auto& title_intro = symbols.find("BG_TITLEI");
            const auto& background_lists = symbols.find("BGLISTS");
            if (title_intro.empty() || background_lists.empty()
                || (title_intro.front() & 0xff0000U)
                    != (background_lists.front() & 0xff0000U)) {
                return std::uint16_t{};
            }
            return static_cast<std::uint16_t>(
                title_intro.front() - background_lists.front());
        }();
        const auto background_id = [&symbols](const char* name) {
            const auto& entry = symbols.find(name);
            const auto& background_lists = symbols.find("BGLISTS");
            if (entry.empty() || background_lists.empty()
                || (entry.front() & 0xff0000U)
                    != (background_lists.front() & 0xff0000U)) {
                return std::uint16_t{};
            }
            return static_cast<std::uint16_t>(
                entry.front() - background_lists.front());
        };
        const auto space_planet_background = background_id("BG_2_2");
        // Both Macbeth approach/departure lists use the singular "34"
        // planet tilemap, not a repeatable landscape or cloud bank.
        const auto macbeth_approach_background = background_id("BG_3_4B");
        const auto macbeth_departure_background = background_id("BG_3_4D");
        const auto ex_twin_planet_background = background_id("BG_5_4");
        const auto ex_face_planet_background = background_id("BG_6_3H");
        // Original and EX BG_SPECIAL use the same decoded face-planet atlas.
        const auto dimension_background = background_id("BG_SPECIAL");
        constexpr std::array ex_face_planets{
#define SF_FACE_PLANET_REGION(l,t,r,b) starfox::render::BackgroundUniqueRegion{l,t,r,b,0,255,14},
#include "starfox/render/ex_face_planet_regions.inc"
#undef SF_FACE_PLANET_REGION
        };
        // EX's 512x512 BG_5_4 tilemap embeds two planets among clouds.
        // Palette 81..86 is planet ink; 88 is sky. The large planet's
        // isolated bounds also include its white crescent (95). The smaller
        // planet shares tiles with clouds, so only its ink is replaced.
        constexpr std::array ex_twin_planets{
            starfox::render::BackgroundUniqueRegion{256, 288, 288, 320, 81, 95, 88},
            starfox::render::BackgroundUniqueRegion{288, 304, 304, 320, 81, 86, 88}};
        const auto special_colour = colour_symbol("ID_1_C");
        const auto red_colour = colour_symbol("RED_C");
        const auto white_colour = colour_symbol("WHITE_C");
        // TRAIL_ISTRAT is only the initializer. Its first invocation changes
        // al_strat to the internal .strat continuation 0x19 bytes later;
        // visible afterimages therefore carry this active address.
        const auto trail_strategy_address =
            symbols.find("TRAIL_ISTRAT").front() + 0x19U;
        const auto flash_player_strategy_address = [&symbols]() {
            const auto& addresses=symbols.find("FLASHPLAYER_STRAT");
            return addresses.empty()?std::uint32_t{}:addresses.front();
        }();
        const auto tunnel_arrow_gate_shape = [&symbols]() {
            const auto& addresses = symbols.find("UP_DOOR");
            return addresses.empty() ? std::uint16_t{}
                : static_cast<std::uint16_t>(addresses.front());
        }();
        const auto ex_crosshair_strategy_address = [&symbols,
                                                      active_experience]() {
            if (active_experience
                != starfox::simulation::Experience::starfox_ex) {
                return std::uint32_t{};
            }
            const auto& addresses = symbols.find("TEST_ISTRAT");
            return addresses.empty() ? std::uint32_t{} : addresses.front();
        }();
        const auto intro_laser_shape = static_cast<std::uint16_t>(
            symbols.find("ELASER2A").front());
        const auto pause_text = [&symbols]() {
            for (const auto address : symbols.find("PAUSETXT")) {
                if ((address & 0xffffU) >= 0x8000U
                    && ((address >> 16U) & 0xffU) < 0x70U) return address;
            }
            throw std::runtime_error{"missing pause text symbol"};
        }();
        const auto game_text_symbol = [&symbols](const char* name) {
            const auto find_text = [&symbols](const char* candidate) {
                for (const auto address : symbols.find(candidate)) {
                    if ((address & 0xffffU) >= 0x8000U
                        && ((address >> 16U) & 0xffU) < 0x70U) return address;
                }
                return std::uint32_t{};
            };
            if (const auto address = find_text(name); address != 0U) return address;
            const auto requested = std::string_view{name};
            const auto* alias = requested == "PEPPYTXT" ? "BUNNYTXT"
                : requested == "FALCOTXT" ? "COCKTXT"
                : requested == "SLIPPYTXT" ? "FROGTXT" : nullptr;
            if (alias != nullptr) {
                if (const auto address = find_text(alias); address != 0U) return address;
            }
            throw std::runtime_error{
                std::string{"missing game text symbol: "} + name};
        };
        const auto score_text = game_text_symbol("SCORETXT");
        const auto total_score_text = game_text_symbol("TOTALSCORETXT");
        const auto team_text = game_text_symbol("TEAMTXT");
        const auto teammate_face_positions = game_text_symbol("NAMEGFXPOS");
        const auto teammate_down_text = game_text_symbol("DEADTXT");
        const std::array teammate_text{
            game_text_symbol("PEPPYTXT"),
            game_text_symbol("FALCOTXT"),
            game_text_symbol("SLIPPYTXT"),
        };

        AudioOutput audio{msu1_pack};
#if defined(STARFOX_UWP)
        log_uwp_startup("audio device opened");
#endif
        audio.set_volumes(game.music_volume(), game.sfx_volume());
        audio.set_msu1_enabled(game.msu1_music()
            && active_experience
                == starfox::simulation::Experience::original);
        // DO_BGM_INIT is part of the cartridge's boot sequence and completes
        // before a level is selected. A command-line level starts its stage
        // bank on the very first logic tick; previously both complete IPL
        // transfers were handed to the SPC emulator in one 50 ms batch. That
        // restarted a driver which had never been allowed to initialize its
        // base sound0 workspace, leaving effects backed by incomplete state.
        // Advance every captured upload separately, without queueing inaudible
        // preroll, so direct-map audio follows the same base-bank -> stage-bank
        // order and timing as the ordinary title/map flow. In particular, the
        // base driver gets one complete SPC frame before the level bank's $ff
        // restart snapshots and overlays its initialized ARAM workspace.
        const auto boot_audio_writes = game.map().take_apu_port_writes();
        if (!boot_audio_writes.empty()) {
            auto ports = audio.prime_upload_sequence(boot_audio_writes);
            std::string direct_entry_name = initial_map;
            std::ranges::transform(direct_entry_name, direct_entry_name.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::toupper(character));
                });
            const auto direct_level_entry = direct_entry_name != "BOOT";
            if (direct_level_entry) {
                // A normal launch leaves sound0 running throughout the title,
                // controls and planet flow before a level bank overlays its
                // initialized ARAM workspace. A CLI map skips that wall-clock
                // interval. Advance the base driver silently for the same
                // 1.5 seconds as the desktop's opening black preroll so the
                // first stage upload never lands on half-initialized state.
                constexpr std::size_t direct_entry_settle_ticks = 30U;
                for (std::size_t tick = 0U;
                     tick < direct_entry_settle_ticks; ++tick) {
                    ports = audio.queue_logic_tick({}, {}, 1U, false);
                }
            }
            game.synchronize_apu_output_ports(ports);
        }
        auto gamepads = starfox::app::open_player_gamepads();
        const auto log_gamepads = [&] {
#if defined(STARFOX_UWP)
            int joystick_count = 0;
            auto* identifiers = SDL_GetJoysticks(&joystick_count);
            SDL_free(identifiers);
            log_uwp_startup("Controller scan: WGI="
                + std::to_string(SDL_GetHintBoolean(SDL_HINT_JOYSTICK_WGI, false))
                + " joysticks=" + std::to_string(joystick_count)
                + " opened gamepads=" + std::to_string(gamepads.size()));
            for (auto* opened : gamepads) {
                log_uwp_startup("Controller: "
                    + starfox::app::gamepad_device_label(opened));
            }
            if (gamepads.empty()) log_uwp_startup(
                std::string{"Controller status: "} + SDL_GetError());
#endif
        };
        log_gamepads();
#if defined(STARFOX_UWP)
        bool logged_controller_input = false;
#endif
        SDL_Gamepad* gamepad = gamepads.empty() ? nullptr : gamepads.front();
        RumbleOutput rumble{symbols};
        MsuFadeOutput msu_fade{symbols};
        const auto close_gamepads = [&] {
            rumble.stop(gamepad);
            for (auto* opened : gamepads) {
                if (opened != nullptr) SDL_CloseGamepad(opened);
            }
            gamepads.clear();
            gamepad = nullptr;
        };
        const auto refresh_gamepads = [&] {
            close_gamepads();
            gamepads = starfox::app::open_player_gamepads();
            gamepad = gamepads.empty() ? nullptr : gamepads.front();
            log_gamepads();
        };
        starfox::app::InputBindings bindings;
        bindings.load();
        const auto hud_layout_path = starfox::app::hud_layout_settings_path();
        starfox::render::HudLayoutProfiles hud_layouts{};
        static_cast<void>(starfox::app::load_hud_layout(
            hud_layout_path, hud_layouts));
        std::array<starfox::input::InputLatch, 4> secondary_inputs{};
        starfox::input::InputLatch remap_input;
        RemapMenuState remap_menu;
        HudEditorState hud_editor;
        hud_editor.active = hud_editor_preview;
        bool running = true;
        bool exit_confirmation =
            std::getenv("STARFOX_TEST_EXIT_CONFIRMATION") != nullptr;
        bool exit_yes_selected{};
        const auto save_hud_layout = [&] {
            static_cast<void>(starfox::app::save_hud_layout(
                hud_layout_path, hud_layouts));
        };
        const auto close_hud_editor = [&] {
            save_hud_layout();
            hud_editor.active = false;
            hud_editor.dragging.reset();
            if (hud_editor_preview) {
                initial_map = "BOOT";
                restart_runtime = true;
                running = false;
            }
        };
        PresentationPacer pacer;
        starfox::timing::RasterPhaseClock raster_clock;
        starfox::timing::RasterPhaseClock frame_step_clock;
        starfox::timing::FixedStepClock realtime_raster_clock{
            starfox::timing::kPresentationHz};
        // Standard presentation keeps the complete 256x224 PPU raster.
        // Widescreen grows the scene symmetrically to 400x224 while HUD and
        // dialogue retain their original 224x192 coordinates in a centred
        // inset layer.
        auto render_scale = render_scale_factor(game.render_scale());
        starfox::render::Framebuffer framebuffer{
            snes_width, snes_height, render_scale};
        starfox::render::Framebuffer superfx_frame{
            snes_width, superfx_height, render_scale};
        starfox::render::SurfaceBuffer superfx_surfaces{
            snes_width * render_scale, superfx_height * render_scale};
        starfox::render::shadows::Scene shadow_scene;
        starfox::render::RowWorkers shadow_workers;
        starfox::render::shadows::DxrShadows dxr_shadows;
        starfox::render::shadows::PortableShadows portable_shadows;
        starfox::render::GpuRaster gpu_raster;
        starfox::render::RasterCommands raster_commands;
        starfox::render::GpuSceneRecording recorded_scene;
        std::vector<starfox::render::GpuModelDraw> controls_model_draws;
        bool gpu_raster_failed=false,gpu_raster_reported=false;
        std::string shadow_backend_status;
        std::vector<std::uint8_t> shadow_mask;
        std::array<std::vector<std::uint8_t>,2> stereo_shadow_masks;
        starfox::render::Framebuffer superfx_ui{
            superfx_ui_width, superfx_height, render_scale};
        starfox::render::Framebuffer comms_hud{
            superfx_ui_width, superfx_height, render_scale};
        starfox::render::Framebuffer superfx_hud{
            snes_width, superfx_height, render_scale};
        starfox::render::Framebuffer controls_player_layer{
            snes_width, superfx_height, render_scale};
        starfox::render::Framebuffer native_ex_overlay{
            snes_width, snes_height};
        // EX's BG1 diagnostics are cartridge art staged at the source raster,
        // but this layer keeps a draw scale of 1 so the usual derivation would
        // read it as geometry. Pin the tag instead.
        native_ex_overlay.set_layer_override(starfox::render::PixelLayer::two_d);
        starfox::render::Framebuffer planet_overlay{snes_width, snes_height};
        starfox::render::Framebuffer setup_overlay{snes_width, snes_height};
        starfox::render::Framebuffer planet_text_overlay{
            snes_width, snes_height};
        starfox::render::Framebuffer live_fps_overlay{64U, 12U};
        starfox::render::Framebuffer exit_confirmation_overlay{112U, 40U};
        starfox::render::Framebuffer mode2_background_cache{
            snes_width, snes_height};
        starfox::simulation::SnesPpuState mode2_background_ppu;
        std::array<std::uint16_t, 32U> mode2_background_vertical{};
        std::int32_t mode2_background_x{};
        std::int32_t mode2_background_y{};
        std::uint64_t mode2_background_source_frame{};
        std::uint64_t mode2_background_scene_revision{};
        std::uint16_t mode2_background_id{};
        bool mode2_background_valid{};
        std::uint64_t mode2_background_temporal_hits{};
        std::uint64_t mode2_background_exact_hits{};
        std::uint64_t mode2_background_misses{};
        starfox::render::Framebuffer cartridge_layer_cache{
            snes_width, snes_height};
        std::uint64_t cartridge_layer_scene_revision{};
        std::uint16_t cartridge_layer_background_id{};
        std::uint8_t cartridge_layer_background_mode{};
        std::uint8_t cartridge_layer_flow_state{};
        bool cartridge_layer_valid{};
        std::uint64_t cartridge_layer_temporal_hits{};
        std::uint64_t cartridge_layer_misses{};
        std::array<std::uint64_t, 8U> profiled_background_modes{};
        std::uint64_t profiled_gameplay_hud_frames{};
        starfox::render::RenderSettings render_settings;
        render_settings.colour_index_base = 7U * 16U;
        render_settings.render_scale = render_scale;
        starfox::render::SoftwareRenderer renderer{render_settings};
        const starfox::render::ParticleRenderer particle_renderer;
        starfox::render::ScaledTextRenderer text_renderer{rom, symbols};
        RecordingBackgroundRenderer background_renderer;
        const starfox::render::DustRenderer dust_renderer{rom, symbols};
        const starfox::render::SpriteRenderer sprite_renderer;
        const auto capture = [&game, &trigonometry]() {
            return starfox::render::capture_object_snapshots(
                game.objects(), trigonometry);
        };
        auto previous = capture();
        auto current = previous;
        const auto capture_camera = [&game, camera_x_address, camera_y_address,
                                     camera_z_address, camera_pitch_address,
                                     camera_yaw_address, camera_roll_address]() {
            return starfox::timing::TransformSnapshot{
                static_cast<std::int16_t>(game.map().read_native_word(camera_x_address)),
                static_cast<std::int16_t>(game.map().read_native_word(camera_y_address)),
                static_cast<std::int16_t>(game.map().read_native_word(camera_z_address)),
                game.map().read_native_word(camera_pitch_address),
                game.map().read_native_word(camera_yaw_address),
                game.map().read_native_word(camera_roll_address)};
        };
        auto previous_camera = capture_camera();
        auto current_camera = previous_camera;
        const auto capture_view_float = [&]() {
            return static_cast<std::int16_t>(game.map().read_native_word(camera_float_y_address));
        };
        auto previous_view_float = capture_view_float();
        auto current_view_float = previous_view_float;
        struct RasterMotionSnapshot {
            std::uint16_t background{};
            std::uint8_t background_mode{};
            std::uint8_t main_screen{};
            bool bg1_tile_size_16{};
            bool bg2_tile_size_16{};
            bool bg3_tile_size_16{};
            bool bg2_vertical_offsets_enabled{};
            bool bg2_horizontal_offsets_enabled{};
            std::uint16_t bg1_character_base{};
            std::uint16_t bg1_screen_base{};
            std::uint16_t bg2_character_base{};
            std::uint16_t bg2_screen_base{};
            std::uint16_t bg3_character_base{};
            std::uint16_t bg3_screen_base{};
            std::int16_t background_x{};
            std::int16_t background_y{};
            std::int16_t bg2_scroll_x{};
            std::int16_t bg2_scroll_y{};
            std::int16_t bg1_scroll_x{};
            std::int16_t bg1_scroll_y{};
            std::int16_t bg3_scroll_x{};
            std::int16_t bg3_scroll_y{};
            std::array<std::int16_t, 224> bg2_horizontal_offsets{};
            std::array<std::uint16_t, 32> bg2_vertical_offsets{};
        };
        const auto capture_raster_motion = [&game, background_x_address,
                                             background_y_address]() {
            const auto& ppu = game.map().ppu_state();
            const auto override_scroll = game.map().background_scroll_override();
            auto snapshot = RasterMotionSnapshot{
                game.map().background(),
                ppu.background_mode,
                ppu.main_screen,
                ppu.bg1_tile_size_16,
                ppu.bg2_tile_size_16,
                ppu.bg3_tile_size_16,
                ppu.bg2_vertical_offsets_enabled,
                ppu.bg2_horizontal_offsets_enabled,
                ppu.bg1_character_base,
                ppu.bg1_screen_base,
                ppu.bg2_character_base,
                ppu.bg2_screen_base,
                ppu.bg3_character_base,
                ppu.bg3_screen_base,
                override_scroll ? (*override_scroll)[0] : static_cast<std::int16_t>(
                    game.map().read_native_word(background_x_address)),
                override_scroll ? (*override_scroll)[1] : static_cast<std::int16_t>(
                    game.map().read_native_word(background_y_address)),
                ppu.bg2_scroll_x,
                ppu.bg2_scroll_y,
                ppu.bg1_scroll_x,
                ppu.bg1_scroll_y,
                ppu.bg3_scroll_x,
                ppu.bg3_scroll_y,
                ppu.bg2_horizontal_offsets,
            };
            for (std::size_t index = 0;
                 index < snapshot.bg2_vertical_offsets.size(); ++index) {
                const auto byte = (0x2fa0U + index) * 2U;
                snapshot.bg2_vertical_offsets[index] =
                    static_cast<std::uint16_t>(ppu.vram[byte])
                    | (static_cast<std::uint16_t>(ppu.vram[byte + 1U]) << 8U);
            }
            return snapshot;
        };
        const auto raster_source_changed = [](
            const RasterMotionSnapshot& previous,
            const RasterMotionSnapshot& current,
            bool ignore_superfx_page_flip) {
            return previous.background != current.background
                || previous.background_mode != current.background_mode
                || previous.main_screen != current.main_screen
                || previous.bg1_tile_size_16 != current.bg1_tile_size_16
                || previous.bg2_tile_size_16 != current.bg2_tile_size_16
                || previous.bg3_tile_size_16 != current.bg3_tile_size_16
                || previous.bg2_vertical_offsets_enabled
                    != current.bg2_vertical_offsets_enabled
                || previous.bg2_horizontal_offsets_enabled
                    != current.bg2_horizontal_offsets_enabled
                || (!ignore_superfx_page_flip
                    && previous.bg1_character_base
                        != current.bg1_character_base)
                || previous.bg1_screen_base != current.bg1_screen_base
                || previous.bg2_character_base != current.bg2_character_base
                || previous.bg2_screen_base != current.bg2_screen_base
                || previous.bg3_character_base != current.bg3_character_base
                || previous.bg3_screen_base != current.bg3_screen_base;
        };
        auto previous_raster_motion = capture_raster_motion();
        auto current_raster_motion = previous_raster_motion;
        auto previous_oam = game.map().ppu_state().oam;
        auto current_oam = previous_oam;
        auto previous_cockpit_roll = game.map().read_native_word(hud_rotation_address);
        auto current_cockpit_roll = previous_cockpit_roll;
        auto previous_circle = game.circle_effect_state();
        auto current_circle = previous_circle;
        auto previous_window_wipe = game.window_wipe_state();
        auto current_window_wipe = previous_window_wipe;
        std::unordered_map<std::uint32_t, starfox::assets::Shape> shape_cache;
        // Geometry counts are decoded before simulation pacing, independently
        // of the presentation cache and its visibility/graphics settings.
        std::unordered_map<std::uint32_t, std::uint32_t> shape_face_counts;
        std::unordered_set<std::uint32_t> invalid_pace_shapes;
        game.set_shape_face_counts(&shape_face_counts);
        std::unordered_set<std::uint32_t> invalid_shapes;
        std::uint64_t presented_frames = 0;
        std::uint64_t source_logic_frames = 0;
        std::uint64_t profile_scene_cuts{};
        std::uint64_t profile_camera_cuts{};
        std::uint64_t profile_raster_cuts{};
        std::uint64_t profile_fractional_presentations{};
        std::uint64_t profile_background_ns{};
        std::uint64_t profile_world_ns{};
        std::uint64_t profile_composite_ns{};
        std::uint64_t profile_present_ns{};
        const auto test_frames_text = std::getenv("STARFOX_TEST_FRAMES");
        const auto test_frames = test_frames_text == nullptr
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(std::stoull(test_frames_text));
        const bool profile_distribution = test_frames != 0U
            && std::getenv("STARFOX_TRACE_PROFILE_DISTRIBUTION") != nullptr;
        const auto* profile_warmup_text=std::getenv("STARFOX_TEST_PROFILE_WARMUP");
        const auto profile_warmup=test_frames && profile_warmup_text
            ?std::min(test_frames,std::uint64_t(std::stoull(profile_warmup_text))):0U;
        std::uint64_t profile_measured_frames{};
        std::vector<std::uint64_t> profile_render_samples;
        if (profile_distribution) profile_render_samples.reserve(test_frames);
        const auto capture_path_text = std::getenv("STARFOX_CAPTURE_PATH");
        const auto capture_path = capture_path_text == nullptr
            ? std::filesystem::path{} : std::filesystem::path{capture_path_text};
        const bool capture_results = test_frames && std::getenv("STARFOX_CAPTURE_RESULTS") != nullptr;
        std::uint64_t capture_results_visible_frames{};
        const auto capture_directory_text = std::getenv("STARFOX_CAPTURE_DIR");
        const auto capture_directory = capture_directory_text == nullptr
            ? std::filesystem::path{}
            : std::filesystem::path{capture_directory_text};
        if (!capture_directory.empty()) {
            std::filesystem::create_directories(capture_directory);
        }
        const auto capture_start_text = std::getenv("STARFOX_CAPTURE_START");
        const auto capture_start = capture_start_text == nullptr
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(std::stoull(capture_start_text));
        const auto* capture_interval_text = std::getenv("STARFOX_CAPTURE_INTERVAL");
        const auto capture_interval = capture_interval_text == nullptr ? 1ULL
            : std::max(1ULL, std::stoull(capture_interval_text));
        const auto scripted_presses = parse_scripted_presses(
            std::getenv("STARFOX_TEST_PRESSES"));
        const auto scripted_press_frames=std::getenv("STARFOX_TEST_PRESS_FRAMES")
            ?std::clamp(std::atoi(std::getenv("STARFOX_TEST_PRESS_FRAMES")),1,240):3;
        const auto test_unpaced = std::getenv("STARFOX_TEST_UNPACED") != nullptr;
        const auto test_fast_forward =
            std::getenv("STARFOX_TEST_FAST_FORWARD") != nullptr;
        std::vector<starfox::simulation::ApuPortWrite> pending_audio_writes;
        std::vector<starfox::simulation::MsuRegisterWrite> pending_msu_writes;
        std::uint8_t audio_video_phases{};
        MouseCameraState mouse_camera;
        ExMouseInputLatch ex_mouse_input;
        TouchControls touch_controls;
        std::uint32_t launch_wipe_reveal_frames{};
        bool window_focused = true;
        bool frame_frozen{};
        FrameStepRepeater frame_step_repeater;
        std::optional<bool> volume_slider_drag_music;
        bool suppress_fullscreen_start{};
        double last_phase_fraction{};
        std::uint8_t state_slot{};
        bool state_slot_window{};
        bool suppress_state_input{};
        starfox::input::InputLatch state_navigation;
        const auto scripted_state_actions = parse_scripted_presses(
            test_frames ? std::getenv("STARFOX_TEST_STATE_ACTIONS") : nullptr);
        const auto state_rom_crc = starfox::assets::crc32(rom.bytes());
        const auto state_slot_path = [&] {
            const auto* test_directory = test_frames ? std::getenv("STARFOX_TEST_STATE_DIRECTORY") : nullptr;
            const auto directory = test_directory ? std::filesystem::path{test_directory}
                : starfox::app::starfox_ex_save_ram_path().parent_path() / "states";
            return directory
                / (std::to_string(state_rom_crc) + "-" + std::to_string(state_slot) + ".sfe");
        };
#if defined(STARFOX_UWP)
        log_uwp_startup("starting first-frame preroll");
#endif

        // Open and synchronize the native window before the cartridge flow
        // begins. This leaves a stable one-and-a-half-second black preroll instead of
        // allowing ROM loading or the first APU upload to race the desktop
        // compositor and become audible/visible before the window appears.
        if (first_runtime
            && std::getenv("STARFOX_TEST_SKIP_PREROLL") == nullptr) {
            framebuffer.clear(0U);
            std::array<starfox::render::Rgba8, 256> startup_palette{};
            PresentationPacer startup_pacer;
            for (std::uint32_t frame = 0; frame < 90U; ++frame) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_EVENT_QUIT) running = false;
                }
                if (!running) break;
                if (!test_unpaced) startup_pacer.wait_for_next_frame();
                window.present(framebuffer, startup_palette, {});
#if defined(STARFOX_UWP)
                if (frame == 0) log_uwp_startup("first frame presented");
#endif
            }
            first_runtime = false;
        }
        // The HUD editor is a frozen visual workspace. Its hidden cartridge
        // preroll must not leak stage music or effects into the options menu.
        if (running && !hud_editor_preview && !menu_preview) audio.start();
#if defined(STARFOX_UWP)
        log_uwp_startup("entering main loop");
        bool uwp_first_runtime_frame = true;
#endif
        auto raster_timestamp = std::chrono::steady_clock::now();
        starfox::timing::LiveFpsCounter live_fps{
            std::chrono::milliseconds{250}};
        live_fps.reset(raster_timestamp, game.presentation_fps());
        while (running) {
            if(test_frames && std::getenv("STARFOX_TEST_REVIVAL")
                && std::getenv("STARFOX_TEST_REVIVAL_FRAME")
                && presented_frames==std::stoull(std::getenv("STARFOX_TEST_REVIVAL_FRAME"))) {
                const auto bank=symbols.find("MAPRESTARTBANK").at(0);
                const auto position=symbols.find("MAPRESTART").at(0);
                if(!game.objects().is_active(game.player())
                    || (!game.map().read_native_byte(bank) && !game.map().read_native_word(position)))
                    throw std::runtime_error("Scheduled revival requires an active player and initialized checkpoint");
                game.map().write_native_byte(symbols.find("LIVES").at(0),2U);
                game.objects().at(game.player()).strategy_address=symbols.find("PLAYERDEAD_ISTRAT").at(0);
                std::cerr<<"scheduled-revival frame="<<presented_frames<<'\n';
            }
            window.update_temporary_status();
            for (const auto& action : scripted_state_actions) {
                if (action.presentation_frame != presented_frames) continue;
                SDL_Event key{};
                key.type = SDL_EVENT_KEY_DOWN;
                key.key.down = true;
                key.key.mod = action.buttons <= 3 ? SDL_KMOD_CTRL : SDL_KMOD_NONE;
                key.key.scancode = action.buttons == 1 ? SDL_SCANCODE_F1
                    : action.buttons == 2 ? SDL_SCANCODE_F2
                    : action.buttons == 3 ? SDL_SCANCODE_F3
                    : action.buttons == 4 ? SDL_SCANCODE_RIGHT
                    : action.buttons == 6 ? SDL_SCANCODE_F1 : SDL_SCANCODE_RETURN;
                SDL_PushEvent(&key);
            }
            bool toggle_frame_freeze{};
            bool step_frame_forward{};
            bool step_frame_backward{};
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat
                    && event.key.scancode==SDL_SCANCODE_F1
                    && (event.key.mod&(SDL_KMOD_CTRL|SDL_KMOD_ALT|SDL_KMOD_SHIFT))==0
                    && !remap_menu.active && !hud_editor.active && !state_slot_window && !exit_confirmation && !frame_frozen) {
                    if(game.toggle_runtime_options()) {
                        audio.set_paused(game.runtime_options_open());
                        input.reset();
                        if(test_frames) std::cerr<<"runtime-options open="<<game.runtime_options_open()
                            <<" experience="<<unsigned(game.experience())<<'\n';
                    }
                    continue;
                }
                if (!remap_menu.active && !hud_editor.active && !frame_frozen && !exit_confirmation
                    && !game.runtime_options_open()
                    && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
                    && (event.key.mod & SDL_KMOD_CTRL) != 0
                    && (event.key.mod & (SDL_KMOD_ALT | SDL_KMOD_SHIFT)) == 0
                    && event.key.scancode >= SDL_SCANCODE_F1
                    && event.key.scancode <= SDL_SCANCODE_F3) {
                    try {
                        if (event.key.scancode == SDL_SCANCODE_F3) {
                            state_slot_window = !state_slot_window;
                            audio.set_paused(state_slot_window);
                            suppress_state_input = true;
                            state_navigation.reset(bindings.sample_gamepad_only(gamepad));
                            input.reset();
                        } else if (event.key.scancode == SDL_SCANCODE_F1) {
                            starfox::state::Writer out;
                            std::vector<std::array<std::uint32_t,3>> apu, msu;
                            for (const auto& w : pending_audio_writes) apu.push_back({w.port,w.value,w.clock_offset});
                            for (const auto& w : pending_msu_writes) msu.push_back({w.address,w.value,w.clock_offset});
                            out(game.save_state(), audio.save_state(), apu, msu,
                                audio_video_phases, source_logic_frames);
                            starfox::state::write_atomic(state_slot_path(),
                                starfox::state::pack(0x52554e01U, state_rom_crc, out.bytes()));
                            window.show_temporary_status("SAVED SLOT " + std::to_string(state_slot));
                            if (test_frames) std::cerr << "state saved slot=" << unsigned(state_slot) << '\n';
                        } else {
                            const auto bytes = starfox::state::read_file(state_slot_path());
                            starfox::state::Reader in{starfox::state::unpack(bytes, 0x52554e01U, state_rom_crc)};
                            std::vector<std::uint8_t> game_bytes, audio_bytes;
                            std::vector<std::array<std::uint32_t,3>> apu, msu;
                            std::uint8_t phases{}; std::uint64_t logic_frames{};
                            in(game_bytes, audio_bytes, apu, msu, phases, logic_frames); in.finish();
                            if (phases >= 3) throw std::runtime_error{"Invalid saved audio phase"};
                            std::vector<starfox::simulation::ApuPortWrite> apu_writes;
                            std::vector<starfox::simulation::MsuRegisterWrite> msu_writes;
                            for (const auto& w : apu) {
                                if (w[0] > 3 || w[1] > 255) throw std::runtime_error{"Invalid saved APU command"};
                                apu_writes.push_back({static_cast<std::uint8_t>(w[0]),static_cast<std::uint8_t>(w[1]),w[2]});
                            }
                            for (const auto& w : msu) {
                                if (w[0] < 0x2000 || w[0] > 0x2007 || w[1] > 255) throw std::runtime_error{"Invalid saved MSU command"};
                                msu_writes.push_back({static_cast<std::uint16_t>(w[0]),static_cast<std::uint8_t>(w[1]),w[2]});
                            }
                            auto restored = game.restored_state(game_bytes);
                            auto restored_audio = audio.prepare_state(audio_bytes);
                            audio.commit_state(std::move(restored_audio));
                            game.swap_state(*restored);
                            window.reset_temporal_history();
                            pending_audio_writes = std::move(apu_writes);
                            pending_msu_writes = std::move(msu_writes);
                            audio_video_phases = phases; source_logic_frames = logic_frames;
                            previous = current = capture();
                            previous_camera = current_camera = capture_camera();
                            previous_view_float = current_view_float = capture_view_float();
                            previous_raster_motion = current_raster_motion = capture_raster_motion();
                            previous_oam = current_oam = game.map().ppu_state().oam;
                            previous_cockpit_roll = current_cockpit_roll = game.map().read_native_word(hud_rotation_address);
                            previous_circle = current_circle = game.circle_effect_state();
                            previous_window_wipe = current_window_wipe = game.window_wipe_state();
                            mode2_background_valid = cartridge_layer_valid = false;
                            if (presentation_history) presentation_history.emplace();
                            raster_clock.reset(); realtime_raster_clock.reset();
                            raster_timestamp = std::chrono::steady_clock::now(); last_phase_fraction = 0;
                            input.reset(); for (auto& latch : secondary_inputs) latch.reset();
                            ex_mouse_input = {}; launch_wipe_reveal_frames = 0;
                            suppress_state_input = true;
                            window.show_temporary_status("LOADED SLOT " + std::to_string(state_slot));
                            if (test_frames) std::cerr << "state loaded slot=" << unsigned(state_slot) << '\n';
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "state operation failed: " << error.what() << '\n';
                        window.show_temporary_status("STATE FAILED: " + std::string{error.what()});
                    }
                    continue;
                }
                if (state_slot_window && event.type == SDL_EVENT_KEY_DOWN) {
                    if (!event.key.repeat) {
                        if (event.key.scancode == SDL_SCANCODE_LEFT || event.key.scancode == SDL_SCANCODE_UP)
                            state_slot = (state_slot + 9U) % 10U;
                        if (event.key.scancode == SDL_SCANCODE_RIGHT || event.key.scancode == SDL_SCANCODE_DOWN)
                            state_slot = (state_slot + 1U) % 10U;
                        if (event.key.scancode == SDL_SCANCODE_ESCAPE || event.key.scancode == SDL_SCANCODE_RETURN) {
                            state_slot_window = false; audio.set_paused(false); input.reset(); suppress_state_input = true;
                        }
                    }
                    continue;
                }
                // Capture the reset suffix before host hotkeys (F-keys,
                // fullscreen and reset itself) can act on that key press.
                if (remap_menu.active && remap_menu.waiting_for_input
                    && remap_menu.device == starfox::app::BindingDevice::keyboard
                    && remap_menu.action == starfox::app::InputBindings::reset_action
                    && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        remap_menu.waiting_for_input = false;
                    } else if (bindings.bind_reset_key(event.key.scancode)) {
                        bindings.save();
                        remap_menu.waiting_for_input = false;
                    }
                    remap_input.reset(bindings.sample_fixed_menu_navigation(gamepad));
                    continue;
                }
                const auto reset_to_setup_key =
                    event.type == SDL_EVENT_KEY_DOWN
                    && !remap_menu.active
                    && bindings.matches_reset_shortcut(event.key);
                if (event.type == SDL_EVENT_KEY_DOWN
                    && !remap_menu.active
                    && starfox::app::InputBindings::matches_god_mode_shortcut(event.key)) {
                    game.set_god_mode(!game.god_mode());
                    window.show_temporary_status(game.god_mode()
                        ? "GOD MODE ON" : "GOD MODE OFF");
                    continue; // Do not also toggle F12 rewind or a mapped action.
                }
                if (reset_to_setup_key) {
                    // Reconstruct the runtime at BOOT so this works from any
                    // cartridge or host-owned screen, including paused play,
                    // EX native menus and the HUD editor.
                    initial_map = "BOOT";
                    restart_runtime = true;
                    running = false;
                    break;
                }
                const auto fullscreen_key =
                    event.type == SDL_EVENT_KEY_DOWN
                    && !event.key.repeat
                    && (event.key.scancode == SDL_SCANCODE_RETURN
                        || event.key.scancode == SDL_SCANCODE_KP_ENTER)
                    && (event.key.mod & SDL_KMOD_ALT) != 0U;
                if (fullscreen_key) {
                    window.toggle_fullscreen();
                    suppress_fullscreen_start = true;
                } else if (event.type == SDL_EVENT_KEY_UP
                           && (event.key.scancode == SDL_SCANCODE_RETURN
                               || event.key.scancode
                                   == SDL_SCANCODE_KP_ENTER)) {
                    suppress_fullscreen_start = false;
                }
                const auto frame_debug_key = event.type == SDL_EVENT_KEY_DOWN
                    && (event.key.scancode == SDL_SCANCODE_F5
                        || event.key.scancode == SDL_SCANCODE_F6
                        || event.key.scancode == SDL_SCANCODE_F7);
                const auto frame_debug_key_down =
                    frame_debug_key && !event.key.repeat;
                if (frame_debug_key_down
                    && event.key.scancode == SDL_SCANCODE_F5) {
                    toggle_frame_freeze = true;
                } else if (frame_debug_key_down
                           && event.key.scancode == SDL_SCANCODE_F6) {
                    frame_step_repeater.press(
                        FrameStepRepeater::Direction::forward,
                        FrameStepRepeater::clock::now());
                    step_frame_forward = true;
                } else if (frame_debug_key_down
                           && event.key.scancode == SDL_SCANCODE_F7) {
                    frame_step_repeater.press(
                        FrameStepRepeater::Direction::backward,
                        FrameStepRepeater::clock::now());
                    step_frame_backward = true;
                } else if (event.type == SDL_EVENT_KEY_UP
                           && event.key.scancode == SDL_SCANCODE_F6) {
                    frame_step_repeater.release(
                        FrameStepRepeater::Direction::forward);
                } else if (event.type == SDL_EVENT_KEY_UP
                           && event.key.scancode == SDL_SCANCODE_F7) {
                    frame_step_repeater.release(
                        FrameStepRepeater::Direction::backward);
                }
                const auto toggle_rewind_key =
                    event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
                    && event.key.scancode == SDL_SCANCODE_F12
                    && game.in_setup_menu()
                    && game.pregame_page()
                        == starfox::simulation::PregamePage::main;
                if (toggle_rewind_key) {
                    if (presentation_history) presentation_history.reset();
                    else presentation_history.emplace();
                    window.show_temporary_status(presentation_history
                        ? "REWIND ENABLED" : "REWIND DISABLED");
                }
                const auto exit_confirmation_key =
                    event.type == SDL_EVENT_KEY_DOWN
                    && !event.key.repeat
                    && event.key.scancode == SDL_SCANCODE_ESCAPE;
                if (exit_confirmation_key && !hud_editor.active
                    && !remap_menu.active) {
                    if (exit_confirmation) {
                        exit_confirmation = false;
                    } else {
                        exit_confirmation = true;
                        // Default to the non-destructive choice. Keyboard and
                        // controller navigation can move to YES explicitly.
                        exit_yes_selected = false;
                        mouse_camera.active = false;
                        window.set_relative_mouse_mode(false);
                    }
                }
                if (exit_confirmation
                    && event.type == SDL_EVENT_KEY_DOWN
                    && !event.key.repeat && !exit_confirmation_key) {
                    if (event.key.scancode == SDL_SCANCODE_LEFT
                        || event.key.scancode == SDL_SCANCODE_UP) {
                        exit_yes_selected = true;
                    } else if (event.key.scancode == SDL_SCANCODE_RIGHT
                               || event.key.scancode == SDL_SCANCODE_DOWN) {
                        exit_yes_selected = false;
                    } else if (event.key.scancode == SDL_SCANCODE_RETURN
                               || event.key.scancode == SDL_SCANCODE_KP_ENTER
                               || event.key.scancode == SDL_SCANCODE_SPACE) {
                        if (exit_yes_selected) running = false;
                        else exit_confirmation = false;
                    }
                }
                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    window_focused = true;
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    window_focused = false;
                    ex_mouse_input.release();
                    touch_controls.reset();
                    frame_step_repeater.reset();
                    if (volume_slider_drag_music) save_pregame_settings();
                    volume_slider_drag_music.reset();
                } else if (event.type == SDL_EVENT_GAMEPAD_ADDED
                           || event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                    refresh_gamepads();
                    for (std::size_t player = 0;
                         player < secondary_inputs.size(); ++player) {
                        const auto held = player + 1U < gamepads.size()
                            ? bindings.sample_gamepad_only(gamepads[player + 1U])
                            : starfox::input::ButtonMask{};
                        secondary_inputs[player].reset(held);
                    }
                }
                if (event.type == SDL_EVENT_FINGER_DOWN
                    || event.type == SDL_EVENT_FINGER_MOTION) {
                    touch_controls.update(event.tfinger.fingerID,
                        event.tfinger.x, event.tfinger.y);
                } else if (event.type == SDL_EVENT_FINGER_UP
                           || event.type == SDL_EVENT_FINGER_CANCELED) {
                    touch_controls.release(event.tfinger.fingerID);
                }
                if (!hud_editor.active && !remap_menu.active
                    && game.in_setup_menu()
                    && game.pregame_page()
                        == starfox::simulation::PregamePage::options) {
                    constexpr float slider_left = 147.0F;
                    constexpr float slider_right = 235.0F;
                    constexpr float music_top = 130.0F;
                    constexpr float sfx_top = 154.0F;
                    constexpr float slider_bottom_offset = 16.0F;
                    const auto update_volume_slider = [&](float window_x,
                                                          float window_y,
                                                          bool begin_drag) {
                        float logical_x{};
                        float logical_y{};
                        if (!window.window_to_logical(window_x, window_y,
                                logical_x, logical_y)) return;
                        const auto viewport = static_cast<float>((
                            display_width_for(game.display_mode())
                            - snes_width) / 2U);
                        const auto local_x = logical_x - viewport;
                        if (begin_drag) {
                            if (logical_y >= music_top
                                && logical_y <= music_top
                                    + slider_bottom_offset) {
                                volume_slider_drag_music = true;
                            } else if (logical_y >= sfx_top
                                       && logical_y <= sfx_top
                                           + slider_bottom_offset) {
                                volume_slider_drag_music = false;
                            } else {
                                return;
                            }
                        }
                        if (!volume_slider_drag_music) return;
                        const auto percent = static_cast<std::uint8_t>(
                            std::clamp(std::lround((local_x - slider_left)
                                * 100.0F / (slider_right - slider_left)),
                                0L, 100L));
                        if (*volume_slider_drag_music) {
                            game.set_music_volume(percent);
                        } else {
                            game.set_sfx_volume(percent);
                        }
                        audio.set_volumes(
                            game.music_volume(), game.sfx_volume());
                    };
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                        && event.button.button == SDL_BUTTON_LEFT) {
                        update_volume_slider(
                            event.button.x, event.button.y, true);
                    } else if (event.type == SDL_EVENT_MOUSE_MOTION
                               && volume_slider_drag_music) {
                        update_volume_slider(
                            event.motion.x, event.motion.y, false);
                    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                               && event.button.button == SDL_BUTTON_LEFT
                               && volume_slider_drag_music) {
                        update_volume_slider(
                            event.button.x, event.button.y, false);
                        save_pregame_settings();
                        volume_slider_drag_music.reset();
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                           && event.button.button == SDL_BUTTON_LEFT) {
                    volume_slider_drag_music.reset();
                }
                if (hud_editor.active) {
                    const auto editor_width = display_width_for(
                        game.display_mode());
                    auto& editor_layout = hud_layouts[
                        hud_profile_index(
                            game.display_mode(), game.experience())];
                    const auto update_pointer = [&](float x, float y) {
                        float logical_x{};
                        float logical_y{};
                        if (window.window_to_logical(
                                x, y, logical_x, logical_y)) {
                            hud_editor.pointer_x = logical_x;
                            hud_editor.pointer_y = logical_y;
                        }
                    };
                    if (event.type == SDL_EVENT_KEY_DOWN
                        && !event.key.repeat
                        && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        close_hud_editor();
                    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                        update_pointer(event.motion.x, event.motion.y);
                        if (hud_editor.dragging) {
                            const auto element = *hud_editor.dragging;
                            const auto base = default_hud_rect(
                                element, editor_width, game.experience());
                            auto& offset = editor_layout[element];
                            offset.x = static_cast<std::int16_t>(std::lround(
                                hud_editor.pointer_x - hud_editor.grab_x
                                - static_cast<float>(base.x)));
                            offset.y = static_cast<std::int16_t>(std::lround(
                                hud_editor.pointer_y - hud_editor.grab_y
                                - static_cast<float>(base.y)));
                            clamp_hud_element(
                                editor_layout, element, editor_width,
                                game.experience());
                        }
                    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                               && event.button.button == SDL_BUTTON_LEFT) {
                        update_pointer(event.button.x, event.button.y);
                        if (hud_reset_button_rect(editor_width).contains(
                                hud_editor.pointer_x,
                                hud_editor.pointer_y)) {
                            editor_layout = {};
                            save_hud_layout();
                        } else if (hud_done_button_rect(editor_width).contains(
                                       hud_editor.pointer_x,
                                       hud_editor.pointer_y)) {
                            close_hud_editor();
                        } else {
                            std::optional<starfox::render::HudElement> picked;
                            auto picked_area = std::numeric_limits<std::int32_t>::max();
                            for (std::uint8_t value = 0U;
                                 value < static_cast<std::uint8_t>(
                                     starfox::render::HudElement::count);
                                 ++value) {
                                const auto element = static_cast<
                                    starfox::render::HudElement>(value);
                                const auto rect = placed_hud_rect(
                                    element, editor_width, editor_layout,
                                    game.experience());
                                if (!rect.contains(hud_editor.pointer_x,
                                        hud_editor.pointer_y)) continue;
                                const auto area = rect.width * rect.height;
                                if (area < picked_area) {
                                    picked = element;
                                    picked_area = area;
                                }
                            }
                            if (picked) {
                                const auto rect = placed_hud_rect(
                                    *picked, editor_width, editor_layout,
                                    game.experience());
                                hud_editor.dragging = *picked;
                                hud_editor.grab_x = hud_editor.pointer_x
                                    - static_cast<float>(rect.x);
                                hud_editor.grab_y = hud_editor.pointer_y
                                    - static_cast<float>(rect.y);
                            }
                        }
                    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                               && event.button.button == SDL_BUTTON_LEFT) {
                        update_pointer(event.button.x, event.button.y);
                        if (hud_editor.dragging) save_hud_layout();
                        hud_editor.dragging.reset();
                    } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                        if (hud_editor.dragging) save_hud_layout();
                        hud_editor.dragging.reset();
                    }
                }
                const auto mouse_camera_scene = game.flow_state()
                        == starfox::simulation::GameFlowState::gameplay
                    || game.flow_state()
                        == starfox::simulation::GameFlowState::training;
                const auto ex_mouse_owns_event =
                    game.ex_pointing_control_enabled()
                    && !remap_menu.active && !hud_editor.active;
                if (event.type == SDL_EVENT_MOUSE_MOTION
                    && ex_mouse_owns_event) {
                    ex_mouse_input.add_motion(
                        event.motion.xrel, event.motion.yrel);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                           && ex_mouse_owns_event
                           && event.button.button == SDL_BUTTON_LEFT) {
                    ex_mouse_input.set_button(0x01U, true);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                           && ex_mouse_owns_event
                           && event.button.button == SDL_BUTTON_RIGHT) {
                    ex_mouse_input.set_button(0x02U, true);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                           && ex_mouse_owns_event
                           && event.button.button == SDL_BUTTON_MIDDLE) {
                    ex_mouse_input.set_button(0x04U, true);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                           && ex_mouse_owns_event
                           && (event.button.button == SDL_BUTTON_X1
                               || event.button.button == SDL_BUTTON_X2)) {
                    ex_mouse_input.set_button(0x08U, true);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                           && event.button.button == SDL_BUTTON_LEFT) {
                    ex_mouse_input.set_button(0x01U, false);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                           && event.button.button == SDL_BUTTON_RIGHT) {
                    ex_mouse_input.set_button(0x02U, false);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                           && event.button.button == SDL_BUTTON_MIDDLE) {
                    ex_mouse_input.set_button(0x04U, false);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                           && (event.button.button == SDL_BUTTON_X1
                               || event.button.button == SDL_BUTTON_X2)) {
                    ex_mouse_input.set_button(0x08U, false);
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                    && event.button.button == SDL_BUTTON_RIGHT
                    && mouse_camera_scene && !remap_menu.active
                    && !hud_editor.active && !ex_mouse_owns_event) {
                    mouse_camera.active = true;
                    window.set_relative_mouse_mode(true);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                           && event.button.button == SDL_BUTTON_RIGHT
                           && !ex_mouse_owns_event) {
                    mouse_camera.active = false;
                    window.set_relative_mouse_mode(false);
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    mouse_camera.active = false;
                    window.set_relative_mouse_mode(false);
                } else if (event.type == SDL_EVENT_MOUSE_MOTION
                           && mouse_camera.active) {
                    constexpr double mouse_angle_units_per_pixel = 72.0;
                    mouse_camera.yaw_offset += static_cast<double>(
                        event.motion.xrel) * mouse_angle_units_per_pixel;
                    mouse_camera.pitch_offset = std::clamp(
                        mouse_camera.pitch_offset - static_cast<double>(
                            event.motion.yrel) * mouse_angle_units_per_pixel,
                        -16'000.0, 16'000.0);
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL
                           && mouse_camera.active) {
                    constexpr double zoom_units_per_wheel_step = 320.0;
                    mouse_camera.zoom_offset = std::clamp(
                        mouse_camera.zoom_offset - static_cast<double>(
                            event.wheel.y) * zoom_units_per_wheel_step,
                        -1'600.0, 12'000.0);
                }
                if (frame_debug_key || fullscreen_key || !remap_menu.active
                    || event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
                    // Keyboard capture is handled below only while the
                    // remapping screen owns input.
                } else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (remap_menu.waiting_for_input) {
                        remap_menu.waiting_for_input = false;
                    } else {
                        bindings.save();
                        remap_menu.active = false;
                    }
                    remap_input.reset(
                        bindings.sample_fixed_menu_navigation(gamepad));
                } else if (remap_menu.waiting_for_input
                           && remap_menu.device
                               == starfox::app::BindingDevice::keyboard) {
                    bindings.bind_keyboard(
                        remap_menu.action, event.key.scancode);
                    bindings.save();
                    remap_menu.waiting_for_input = false;
                    remap_input.reset(
                        bindings.sample_fixed_menu_navigation(gamepad));
                }
                if (remap_menu.active && remap_menu.waiting_for_input
                    && remap_menu.device
                        == starfox::app::BindingDevice::gamepad
                    && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
                    && (gamepad == nullptr
                        || event.gbutton.which == SDL_GetGamepadID(gamepad))) {
                    bindings.bind_gamepad_button(remap_menu.action,
                        static_cast<SDL_GamepadButton>(event.gbutton.button));
                    bindings.save();
                    remap_menu.waiting_for_input = false;
                    remap_input.reset(
                        bindings.sample_fixed_menu_navigation(gamepad));
                }
                if (remap_menu.active && remap_menu.waiting_for_input
                    && remap_menu.device
                        == starfox::app::BindingDevice::gamepad
                    && event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
                    && (event.gaxis.value >= 24'000
                        || event.gaxis.value <= -24'000)
                    && (gamepad == nullptr
                        || event.gaxis.which == SDL_GetGamepadID(gamepad))) {
                    bindings.bind_gamepad_axis(remap_menu.action,
                        static_cast<SDL_GamepadAxis>(event.gaxis.axis),
                        event.gaxis.value > 0);
                    bindings.save();
                    remap_menu.waiting_for_input = false;
                    remap_input.reset(
                        bindings.sample_fixed_menu_navigation(gamepad));
                }
            }

            if (!running) break;
            window.set_render_options(game.renderer_mode(),
                game.anti_aliasing_mode(),
                game.enhanced_graphics(), game.smooth_polys(),
                game.rtx_lighting_intensity(), game.vsync(),
                two_d_filter_backend(game.two_d_filter()),
                static_cast<starfox::render::Effect>(game.effect()), game.effect_intensity(),
                static_cast<starfox::render::Effect>(game.world_effect()), game.world_effect_intensity(), game.bloom(), game.bloom_2d(), game.model_smoothing());
            if (toggle_frame_freeze) {
                frame_frozen = !frame_frozen;
                input.reset();
                for (std::size_t player = 0;
                     player < secondary_inputs.size(); ++player) {
                    const auto held = player + 1U < gamepads.size()
                        ? bindings.sample_gamepad_only(gamepads[player + 1U])
                        : starfox::input::ButtonMask{};
                    secondary_inputs[player].reset(held);
                }
                remap_input.reset(
                    bindings.sample_fixed_menu_navigation(gamepad));
                if (presentation_history) presentation_history->to_live();
                if (frame_frozen) {
                    frame_step_clock.synchronize(last_phase_fraction);
                    audio.set_paused(true);
                    mouse_camera.active = false;
                    window.set_relative_mouse_mode(false);
                    window.set_frame_debug_status(true,
                        presentation_history
                            ? presentation_history->cursor() : 0U,
                        presentation_history
                            ? presentation_history->frame_count() : 0U);
                } else {
                    if (const auto* live = presentation_history
                            ? presentation_history->current() : nullptr) {
                        window.present_rgba(
                            live->width, live->height, live->rgba);
                    }
                    audio.set_paused(false);
                    window.set_frame_debug_status(false);
                    realtime_raster_clock.reset();
                    raster_timestamp = std::chrono::steady_clock::now();
                    live_fps.reset(raster_timestamp, game.presentation_fps());
                    frame_step_repeater.reset();
                }
            }

            if (frame_frozen) {
                if (const auto repeated = frame_step_repeater.poll(
                        FrameStepRepeater::clock::now())) {
                    step_frame_forward = *repeated
                        == FrameStepRepeater::Direction::forward;
                    step_frame_backward = *repeated
                        == FrameStepRepeater::Direction::backward;
                }
            }

            if (frame_frozen && step_frame_backward) {
                if (presentation_history
                    && presentation_history->step_back()) {
                    const auto* frame = presentation_history->current();
                    window.present_rgba(frame->width, frame->height, frame->rgba);
                    window.set_frame_debug_status(true,
                        presentation_history->cursor(),
                        presentation_history->frame_count());
                }
                continue;
            }
            if (frame_frozen && step_frame_forward
                && presentation_history
                && !presentation_history->at_live()) {
                static_cast<void>(presentation_history->step_forward());
                const auto* frame = presentation_history->current();
                window.present_rgba(frame->width, frame->height, frame->rgba);
                window.set_frame_debug_status(true,
                    presentation_history->cursor(),
                    presentation_history->frame_count());
                continue;
            }
            const auto advance_frozen_frame = frame_frozen && step_frame_forward;
            if (frame_frozen && !advance_frozen_frame) {
                std::this_thread::sleep_for(std::chrono::milliseconds{8});
                continue;
            }

            const auto mouse_camera_scene = game.flow_state()
                    == starfox::simulation::GameFlowState::gameplay
                || game.flow_state()
                    == starfox::simulation::GameFlowState::training;
            const auto ex_mouse_capture = window_focused && !frame_frozen
                && game.ex_pointing_control_enabled()
                && !remap_menu.active && !hud_editor.active
                && !exit_confirmation;
            if (mouse_camera.active
                && (!mouse_camera_scene || hud_editor.active
                    || ex_mouse_capture)) {
                mouse_camera.active = false;
            }
            if (!ex_mouse_capture && (remap_menu.active || hud_editor.active
                    || !game.ex_pointing_control_enabled())) {
                ex_mouse_input.release();
            }
            window.set_relative_mouse_mode(
                ex_mouse_capture || mouse_camera.active);

            if (!test_unpaced && !advance_frozen_frame) {
                pacer.wait_for_next_frame(game.presentation_fps());
            }
            const auto* keyboard_state = SDL_GetKeyboardState(nullptr);
            const auto menu_peek = starfox::app::peek_setup_menu(
                game.in_setup_menu(),
                window_focused && (keyboard_state[SDL_SCANCODE_TAB]
                    || (test_frames != 0U && std::getenv("STARFOX_TEST_MENU_PEEK") != nullptr)),
                hud_editor.active || (remap_menu.active && remap_menu.waiting_for_input));
            if (suppress_fullscreen_start
                && !keyboard_state[SDL_SCANCODE_RETURN]
                && !keyboard_state[SDL_SCANCODE_KP_ENTER]) {
                suppress_fullscreen_start = false;
            }
            auto sampled_buttons = bindings.sample(gamepad);
            state_navigation.sample(bindings.sample_gamepad_only(gamepad));
            const auto state_controls = state_navigation.consume();
            if (state_slot_window) {
                if ((state_controls.pressed & (starfox::input::left | starfox::input::up)) != 0)
                    state_slot = (state_slot + 9U) % 10U;
                if ((state_controls.pressed & (starfox::input::right | starfox::input::down)) != 0)
                    state_slot = (state_slot + 1U) % 10U;
                if ((state_controls.pressed & (starfox::input::a | starfox::input::b | starfox::input::start)) != 0) {
                    state_slot_window = false; audio.set_paused(false); input.reset(); suppress_state_input = true;
                }
            }
#if defined(STARFOX_UWP)
            if (!logged_controller_input && gamepad != nullptr
                && bindings.sample_gamepad_only(gamepad) != 0U) {
                logged_controller_input = true;
                log_uwp_startup("First controller input received");
            }
#endif
            if (game.on_screen_controls()) {
                sampled_buttons = static_cast<ButtonMask>(
                    sampled_buttons | touch_controls.buttons());
            }
            sampled_buttons = with_swapped_face_buttons(
                sampled_buttons, game.swap_face_buttons());
            for (const auto& press : scripted_presses) {
                if (presented_frames >= press.presentation_frame
                    && presented_frames < press.presentation_frame + static_cast<std::uint64_t>(scripted_press_frames)) {
                    sampled_buttons = static_cast<ButtonMask>(
                        sampled_buttons | press.buttons);
                }
            }
            if (suppress_fullscreen_start) {
                sampled_buttons = static_cast<ButtonMask>(
                    sampled_buttons & ~starfox::input::start);
            }
            if (state_slot_window || suppress_state_input) {
                if (!state_slot_window && sampled_buttons == 0) suppress_state_input = false;
                sampled_buttons = 0;
            }
            input.sample(sampled_buttons);
            if (exit_confirmation) {
                // Host confirmation input is presentation-rate UI. Consuming
                // it only inside a 60 Hz raster phase lost quick presses at
                // 120+ FPS, making YES appear unable to exit.
                const auto controls = input.consume();
                if ((controls.pressed & (starfox::input::left
                        | starfox::input::up)) != 0U) {
                    exit_yes_selected = true;
                }
                if ((controls.pressed & (starfox::input::right
                        | starfox::input::down)) != 0U) {
                    exit_yes_selected = false;
                }
                if ((controls.pressed & starfox::input::b) != 0U) {
                    exit_confirmation = false;
                } else if ((controls.pressed & (starfox::input::a
                               | starfox::input::start)) != 0U) {
                    if (exit_yes_selected) running = false;
                    else exit_confirmation = false;
                }
            }
            if (!running) break;
            for (std::size_t player = 0;
                 player < secondary_inputs.size(); ++player) {
                secondary_inputs[player].sample(
                    player + 1U < gamepads.size()
                        ? with_swapped_face_buttons(
                            bindings.sample_gamepad_only(gamepads[player + 1U]),
                            game.swap_face_buttons())
                        : starfox::input::ButtonMask{});
            }
            remap_input.sample(
                bindings.sample_fixed_menu_navigation(gamepad));
            const auto tab_fast_forward = keyboard_state[SDL_SCANCODE_TAB]
                && !game.in_setup_menu()
                && !(remap_menu.active && remap_menu.waiting_for_input);
            const auto control_fast_forward = keyboard_state[SDL_SCANCODE_LCTRL]
                || keyboard_state[SDL_SCANCODE_RCTRL];
            const auto shift_fast_forward = keyboard_state[SDL_SCANCODE_LSHIFT]
                || keyboard_state[SDL_SCANCODE_RSHIFT];
            const auto super_fast_forward = keyboard_state[SDL_SCANCODE_GRAVE]
                && !(remap_menu.active && remap_menu.waiting_for_input);
            const auto speed_multiplier = advance_frozen_frame ? 1U
                : test_fast_forward ? 2U
                : starfox::timing::playback_speed_multiplier(tab_fast_forward,
                      control_fast_forward, shift_fast_forward,
                      super_fast_forward);
            // Presentation FPS is independent of the cartridge's 60 Hz
            // raster. Low output rates may service multiple raster phases
            // before one draw; high rates expose fractional progress between
            // phases for smooth interpolation without accelerating gameplay.
            const auto raster_batch = [&]() {
                if (advance_frozen_frame) {
                    return frame_step_clock.advance(
                        starfox::timing::frame_debug_presentation_hz(
                            game.presentation_fps()));
                }
                if (test_unpaced) {
                    return raster_clock.advance(
                        game.presentation_fps(), speed_multiplier);
                }
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<
                    starfox::timing::FixedStepClock::duration>(
                        now - raster_timestamp);
                raster_timestamp = now;
                const auto realtime = realtime_raster_clock.advance(
                    elapsed * speed_multiplier);
                return starfox::timing::RasterPhaseBatch{
                    realtime.simulation_steps,
                    realtime.interpolation_alpha,
                };
            }();
            last_phase_fraction = raster_batch.phase_fraction;
            for (std::uint32_t phase = 0;
                 phase < raster_batch.video_phases; ++phase) {
                if (hud_editor.active) {
                    const auto editor_controls = remap_input.consume();
                    if ((editor_controls.pressed
                         & starfox::input::y) != 0U) {
                        hud_layouts[hud_profile_index(
                            game.display_mode(), game.experience())] = {};
                        save_hud_layout();
                    }
                    if ((editor_controls.pressed
                         & (starfox::input::b | starfox::input::start)) != 0U) {
                        close_hud_editor();
                    }
                    // Do not advance video phases, strategies, interpolation,
                    // particles, dialogue, or audio while editing. Mouse and
                    // controller editor input remains live around this frozen
                    // cartridge snapshot.
                    continue;
                }
                if (exit_confirmation || state_slot_window) {
                    // Freeze source video, simulation and input underneath
                    // the host confirmation card.
                    continue;
                }
                if (game.timing_mode() == starfox::simulation::TimingMode::original_speed) {
                    // Populate before the pace decision, independent of rendering
                    // FPS, culling, LOD and graphical options. Decode once per shape.
                    for (const auto handle : game.draw_order()) {
                        if (!game.objects().is_active(handle)) continue;
                        const auto shape = game.objects().at(handle).shape;
                        if (shape_face_counts.contains(shape)
                            || invalid_pace_shapes.contains(shape)) continue;
                        try {
                            shape_face_counts.emplace(shape, static_cast<std::uint32_t>(
                                decoder.decode(shape).faces.size()));
                        } catch (const std::exception&) {
                            invalid_pace_shapes.insert(shape);
                        }
                    }
                }
                game.present_frame();
                rumble.advance(game.map(), gamepad,
                    game.rumble()
                    && active_experience
                        == starfox::simulation::Experience::original);
                if (game.msu1_music()
                    && active_experience
                        == starfox::simulation::Experience::original) {
                    if (const auto fade = msu_fade.advance(game.map())) {
                        pending_msu_writes.push_back(*fade);
                    }
                }
                if (game.logic_tick_ready()) {
                    auto controls = input.consume();
                    std::array<starfox::input::TickInput, 4>
                        secondary_controls{};
                    for (std::size_t player = 0;
                         player < secondary_controls.size(); ++player) {
                        secondary_controls[player] =
                            secondary_inputs[player].consume();
                    }
                    const auto remap_controls = remap_input.consume();
                    if (remap_menu.active) {
                        const auto remap_action_count = starfox::app::InputBindings::remap_action_count(remap_menu.device);
                        if (!remap_menu.waiting_for_input) {
                            if ((remap_controls.pressed
                                 & starfox::input::up) != 0U) {
                                remap_menu.action = (remap_menu.action
                                    + remap_action_count
                                    - 1U)
                                    % remap_action_count;
                            } else if ((remap_controls.pressed
                                        & starfox::input::down) != 0U) {
                                remap_menu.action = (remap_menu.action + 1U)
                                    % remap_action_count;
                            }
                            if ((remap_controls.pressed
                                 & (starfox::input::left
                                    | starfox::input::right)) != 0U) {
                                remap_menu.device = remap_menu.device
                                        == starfox::app::BindingDevice::keyboard
                                    ? starfox::app::BindingDevice::gamepad
                                    : starfox::app::BindingDevice::keyboard;
                                remap_menu.action = std::min(remap_menu.action,
                                    starfox::app::InputBindings::remap_action_count(remap_menu.device) - 1U);
                            }
                            if ((remap_controls.pressed
                                 & starfox::input::y) != 0U) {
                                bindings.reset(remap_menu.device);
                                bindings.save();
                            }
                            if ((remap_controls.pressed
                                 & starfox::input::a) != 0U) {
                                remap_menu.waiting_for_input = true;
                            } else if ((remap_controls.pressed
                                        & (starfox::input::b
                                           | starfox::input::start)) != 0U) {
                                bindings.save();
                                remap_menu.active = false;
                            }
                        }
                        controls = {};
                        secondary_controls = {};
                    } else if (game.in_setup_menu()
                               && game.pregame_page()
                                   == starfox::simulation::PregamePage::options
                               && game.pregame_selection() == 8U
                               && (controls.pressed
                                   & (starfox::input::a | starfox::input::b))
                                   != 0U) {
                        remap_menu.active = true;
                        remap_menu.waiting_for_input = false;
                        remap_input.reset(
                            bindings.sample_fixed_menu_navigation(gamepad));
                        controls = {};
                        secondary_controls = {};
                    } else if (game.in_setup_menu()
                               && game.pregame_page()
                                   == starfox::simulation::PregamePage::options
                               && game.pregame_selection() == 3U
                               && (controls.pressed
                                   & (starfox::input::a
                                      | starfox::input::select)) != 0U) {
                        auto& editor_layout = hud_layouts[
                            hud_profile_index(
                                game.display_mode(), game.experience())];
                        clamp_hud_layout(editor_layout,
                            display_width_for(game.display_mode()),
                            game.experience());
                        launch_hud_editor_preview = true;
                        initial_map = "LEVEL1_1";
                        restart_runtime = true;
                        running = false;
                        controls = {};
                        secondary_controls = {};
                    }
                    previous = current;
                    previous_camera = current_camera;
                    previous_view_float = current_view_float;
                    previous_raster_motion = current_raster_motion;
                    previous_oam = current_oam;
                    previous_cockpit_roll = current_cockpit_roll;
                    previous_circle = current_circle;
                    previous_window_wipe = current_window_wipe;
                    const auto previous_scene = game.scene_revision();
                    const auto settings_before_tick = capture_pregame_settings();
                    game.set_secondary_inputs(secondary_controls);
                    game.set_mouse_input(ex_mouse_input.consume());
                    game.set_ntt_input(remap_menu.active || hud_editor.active
                            ? 0U
                            : sample_ntt_data_pad(keyboard_state));
                    const bool trace_scripted_input=!scripted_presses.empty() && controls.pressed!=0;
                    const auto scripted_bomb_count=[&]() -> int {
                        const auto& entries=symbols.find("SPECWEPCNT");
                        return entries.empty()?-1:int(game.map().read_native_word(entries.front()));
                    };
                    const auto scripted_bombs_before=trace_scripted_input
                        ?scripted_bomb_count():0;
                    const bool runtime_options_were_open=game.runtime_options_open();
                    const auto tick_result = game.tick(controls);
                    if(runtime_options_were_open && !game.runtime_options_open()) {
                        audio.set_paused(false);input.reset();
                    }
                    if(trace_scripted_input) {
                        std::cerr<<"scripted-input: frame="<<presented_frames<<" pressed="<<controls.pressed
                            <<" bombs="<<scripted_bombs_before<<"->"<<scripted_bomb_count()<<'\n';
                        for(const auto* name:{"PSHIPFLAGS","STAYBLACK","DOINGWIPE","SPECIALDELAY"}) {
                            const auto& entries=symbols.find(name);
                            if(!entries.empty()) std::cerr<<"scripted-input-gate: "<<name<<'='<<unsigned(game.map().read_native_byte(entries.front()))<<'\n';
                        }
                    }
                    if (game.preview_requested() != menu_preview
                        || game.preview_start_requested()) {
                        save_pregame_settings();
                        launch_menu_preview = game.preview_requested()
                            && !game.preview_start_requested();
                        launch_game_after_preview = game.preview_start_requested();
                        initial_map = launch_menu_preview ? "LEVEL1_1" : "BOOT";
                        restart_runtime = true;
                        running = false;
                        break;
                    }
                    // Cartridge PAUSESND commands still run through the SPC
                    // streams so their pause/unpause effects are audible.
                    // Companion MSU playback is host-decoded, so freeze only
                    // that music cursor at the same gameplay boundary.
                    audio.set_game_paused(game.paused());
                    audio.set_volumes(game.music_volume(), game.sfx_volume());
                    ++source_logic_frames;
                    // EX commits its option pages to $71:f000 inside RESTART.
                    // Mirror that battery-backed bank as soon as the source
                    // changes it so an ordinary window close cannot lose the
                    // just-confirmed cartridge settings.
                    synchronize_ex_save();
                    if (capture_pregame_settings() != settings_before_tick) {
                        save_pregame_settings();
                    }
                    if (game.experience() != active_experience) {
                        active_experience = game.experience();
                        save_pregame_settings();
                        launch_menu_preview = menu_preview;
                        initial_map = menu_preview ? "LEVEL1_1" : "BOOT";
                        restart_runtime = true;
                        running = false;
                        break;
                    }
                    current = capture();
                    current_camera = capture_camera();
                    current_view_float = capture_view_float();
                    current_raster_motion = capture_raster_motion();
                    current_oam = game.map().ppu_state().oam;
                    current_cockpit_roll = game.map().read_native_word(hud_rotation_address);
                    current_circle = game.circle_effect_state();
                    current_window_wipe = game.window_wipe_state();
                    const auto camera_cut =
                        starfox::timing::camera_transform_is_discontinuous(
                            previous_camera, current_camera);
                    const auto host_superfx_gameplay =
                        current_raster_motion.background_mode == 2U
                        && (game.flow_state()
                                == starfox::simulation::GameFlowState::gameplay
                            || game.flow_state()
                                == starfox::simulation::GameFlowState::training);
                    const auto raster_cut = raster_source_changed(
                        previous_raster_motion, current_raster_motion,
                        host_superfx_gameplay);
                    if (game.scene_revision() != previous_scene) {
                        ++profile_scene_cuts;
                    }
                    if (camera_cut) ++profile_camera_cuts;
                    if (raster_cut) {
                        ++profile_raster_cuts;
                    }
                    if (game.scene_revision() != previous_scene || camera_cut) {
                        window.reset_temporal_history();
                        // LEVEL1_1 replaces the scramble camera with ExitBase's
                        // view in one source update. Interpolating that cut put
                        // the newly spawned docking station off-screen, then
                        // huge at the right edge, for two 60 FPS presentations.
                        // Snap the complete presentation state at any such view
                        // discontinuity, just as we already do for scene loads.
                        previous = current;
                        previous_camera = current_camera;
                        previous_view_float = current_view_float;
                        previous_raster_motion = current_raster_motion;
                        previous_oam = current_oam;
                        previous_cockpit_roll = current_cockpit_roll;
                        previous_circle = current_circle;
                        previous_window_wipe = current_window_wipe;
                    } else if (raster_cut) {
                        // EX alternates its Super FX BG1 character page every
                        // source update. That is a normal completed bitmap
                        // transfer, not a camera cut. Snap only the raster
                        // registers whose interpretation changed; retaining
                        // the prior camera/object snapshots keeps the host's
                        // 60-480 Hz presentation interpolation alive.
                        previous_raster_motion = current_raster_motion;
                    }
                    // Runtime setup is a pause, not an inaudible fast-forward.
                    // Preserve pre-menu gameplay writes, but do not replay a
                    // backlog of menu navigation sounds when playback resumes.
                    if(!runtime_options_were_open)
                        pending_audio_writes.insert(pending_audio_writes.end(),
                            tick_result.audio_port_writes.begin(),
                            tick_result.audio_port_writes.end());
                    auto msu_writes = game.map().take_msu_register_writes();
                    pending_msu_writes.insert(pending_msu_writes.end(),
                        msu_writes.begin(), msu_writes.end());
                    audio.set_msu1_enabled(game.msu1_music()
                        && active_experience
                            == starfox::simulation::Experience::original);
                }
                if (!game.runtime_options_open() && ++audio_video_phases >= 3U) {
                    game.synchronize_apu_output_ports(
                        audio.queue_logic_tick(
                            pending_audio_writes, pending_msu_writes,
                            speed_multiplier,
                            !advance_frozen_frame));
                    pending_audio_writes.clear();
                    pending_msu_writes.clear();
                    audio_video_phases = 0U;
                }
            }
            if (restart_runtime) break;
            const auto profile_frame_start = std::chrono::steady_clock::now();
            const auto display_width = display_width_for(game.display_mode());
            auto active_hud_layout = hud_layouts[
                hud_profile_index(game.display_mode(), game.experience())];
            clamp_hud_layout(
                active_hud_layout, display_width, game.experience());
            const auto viewport_origin = static_cast<std::int32_t>(
                (display_width - snes_width) / 2U);
            const auto superfx_ui_offset_x = static_cast<std::int32_t>(
                (display_width - superfx_ui_width) / 2U);
            const auto boss_roll = game.boss_roll_active();
            const auto extend_cartridge_scene = game.flow_state()
                    == starfox::simulation::GameFlowState::intro
                || game.flow_state()
                    == starfox::simulation::GameFlowState::ex_pregame_menu
                || game.flow_state()
                    == starfox::simulation::GameFlowState::gameplay
                || game.flow_state()
                    == starfox::simulation::GameFlowState::training
                || game.flow_state()
                    == starfox::simulation::GameFlowState::stage_results
                || (game.flow_state()
                    == starfox::simulation::GameFlowState::credits && !boss_roll)
                || game.final_score_active();
            const auto gameplay_hud = game.flow_state()
                    == starfox::simulation::GameFlowState::gameplay
                || game.flow_state()
                    == starfox::simulation::GameFlowState::training;
            const auto results = game.stage_results_state();
            const auto stage_hud = gameplay_hud || results.visible;
            const auto present_native_ex_bitmap = game.experience()
                    == starfox::simulation::Experience::starfox_ex
                && (gameplay_hud || results.visible);
            const auto* gameplay_layout = stage_hud
                ? &active_hud_layout : nullptr;
            // Gameplay now exposes the complete 224-line host raster in every
            // aspect ratio. Keeping the 192-line Super FX target at 4:3 left
            // the upper and lower two tile rows unable to receive models even
            // though their BG pixels were visible. Other cartridge scenes
            // retain their original vertical window unless a wide mode is in
            // use; the +16 vanishing-point adjustment preserves screen centre.
            const auto extend_scene_vertical = gameplay_hud
                || (display_width > snes_width && extend_cartridge_scene);
            const auto anchor_edge_hud = display_width > snes_width
                && stage_hud;
            const auto scene_height = extend_scene_vertical
                ? snes_height : superfx_height;
            const auto scene_offset_y = extend_scene_vertical
                ? 0 : superfx_offset_y;
            render_scale = render_scale_factor(game.render_scale());
            if (render_settings.render_scale != render_scale
                || render_settings.wireframe_thickness != 1U) {
                render_settings.render_scale = render_scale;
                render_settings.wireframe_thickness = 1U;
                renderer = starfox::render::SoftwareRenderer{render_settings};
            }
            for (auto* layer : {&framebuffer, &superfx_frame, &superfx_hud,
                     &controls_player_layer, &superfx_ui, &comms_hud}) {
                layer->set_draw_scale(render_scale);
            }
            // Layer tags are what let the 2D filter tell cartridge art from
            // projected geometry. Every framebuffer that feeds the presented
            // one has to carry them, or a composite would erase the
            // distinction; they cost nothing while the filter is off.
            const auto two_d_filter = two_d_filter_backend(game.two_d_filter());
            const auto tag_layers = two_d_filter
                != starfox::render::TwoDFilter::off || game.effect() != 0U || game.world_effect() != 0U || game.bloom() != 0U || game.bloom_2d() != 0U || game.model_smoothing() != 0U
                || game.hdr_effect() != 0U || game.chromatic_aberration() != 0U
                || game.ray_tracing() || window.native_gpu_enabled();
            if (framebuffer.layer_tags_enabled() != tag_layers) {
                // Cached pixels made with filtering off have no ownership tags.
                cartridge_layer_valid = false;
                mode2_background_valid = false;
            }
            for (auto* layer : {&framebuffer, &superfx_frame, &superfx_hud,
                     &controls_player_layer, &superfx_ui, &comms_hud,
                     &native_ex_overlay, &cartridge_layer_cache,
                     &mode2_background_cache}) {
                layer->enable_layer_tags(tag_layers);
            }
            framebuffer.resize(display_width, snes_height);
            superfx_frame.resize(display_width, scene_height);
            // Surface samples parallel the stored 3D raster, so at a high
            // render scale this is the largest per-frame allocation. Only the
            // two surface-driven effects read it; leave it empty otherwise.
            const auto surface_effects = game.smooth_polys()
                || game.rtx_lighting();
            superfx_surfaces.resize(
                surface_effects ? display_width * render_scale : 0U,
                surface_effects ? scene_height * render_scale : 0U);
            superfx_hud.resize(display_width, superfx_height);
            controls_player_layer.resize(display_width, superfx_height);
            // EX's native BG1 diagnostics occupy the cartridge's 256-pixel
            // canvas. Keeping this staging layer as wide as 32:9 needlessly
            // cleared and composited hundreds of thousands of transparent
            // pixels on every high-refresh gameplay presentation.
            native_ex_overlay.resize(snes_width, snes_height);
            superfx_ui.resize(superfx_ui_width, superfx_height);
            comms_hud.resize(superfx_ui_width, superfx_height);
            planet_overlay.resize(display_width, snes_height);
            planet_text_overlay.resize(display_width, snes_height);
            // A paused cartridge presents one completed source frame. Do not
            // keep traversing the fractional interpolation interval while
            // source state is frozen; that made star/dust pixels alternate
            // between adjacent integer projections on high-refresh displays.
            const auto interpolation_alpha = game.paused() ? 1.0
                : game.logic_interpolation_alpha(raster_batch.phase_fraction);
            if (interpolation_alpha > 0.0 && interpolation_alpha < 1.0) {
                ++profile_fractional_presentations;
            }
            const auto interpolate_raster_word = [interpolation_alpha](
                std::int16_t previous_value, std::int16_t current_value) {
                return interpolate_source_word(
                    previous_value, current_value, interpolation_alpha);
            };
            const auto ex_native_menu = game.flow_state()
                == starfox::simulation::GameFlowState::ex_pregame_menu;
            const auto controls_scene = game.flow_state()
                    == starfox::simulation::GameFlowState::controls_type
                || game.flow_state()
                    == starfox::simulation::GameFlowState::controls_choice;
            auto ppu = game.map().ppu_state();
            // Gameplay's Mode 1/2 landscape follows the host-interpolated
            // BG2XSCROLL/BG2SCROLL work variables. Mode 3 front ends do not:
            // PLANETSEQ clears and owns the actual PPU BG2 scroll registers
            // while those gameplay scratch words retain stale values. The
            // controller selector likewise moves among four 256x256 BG2
            // quadrants through SEQSCROLL, so read its live 60 Hz PPU value
            // rather than the unrelated gameplay words.
            const auto use_ppu_bg2_scroll = ex_native_menu
                || game.flow_state()
                    == starfox::simulation::GameFlowState::game_over
                || current_raster_motion.background_mode == 3U;
            const auto background_x = controls_scene || boss_roll
                ? ppu.bg2_scroll_x
                : interpolate_raster_word(
                    use_ppu_bg2_scroll
                        ? previous_raster_motion.bg2_scroll_x
                        : previous_raster_motion.background_x,
                    use_ppu_bg2_scroll
                        ? current_raster_motion.bg2_scroll_x
                        : current_raster_motion.background_x);
            const auto background_y = controls_scene || boss_roll
                ? ppu.bg2_scroll_y
                : !use_ppu_bg2_scroll
                ? static_cast<std::int16_t>(starfox::timing::interpolate_wrapped_scroll(
                    previous_raster_motion.background_y,
                    current_raster_motion.background_y, interpolation_alpha, 0x1ffU))
                : interpolate_raster_word(
                    use_ppu_bg2_scroll
                        ? previous_raster_motion.bg2_scroll_y
                        : previous_raster_motion.background_y,
                    use_ppu_bg2_scroll
                        ? current_raster_motion.bg2_scroll_y
                        : current_raster_motion.background_y);
            if (game.flow_state()
                    == starfox::simulation::GameFlowState::gameplay
                || game.flow_state()
                    == starfox::simulation::GameFlowState::training) {
                starfox::render::interpolate_crosshair_oam(
                    previous_oam, interpolation_alpha, ppu);
            } else {
                starfox::render::suppress_crosshair_oam(ppu);
            }
            const auto ex_title_logo_screen = game.experience()
                    == starfox::simulation::Experience::starfox_ex
                && game.flow_state()
                    == starfox::simulation::GameFlowState::title
                && ex_title_intro_background != 0U
                && game.map().background() == ex_title_intro_background;
            const auto extend_ex_title_art = display_width > snes_width
                && game.experience()
                    == starfox::simulation::Experience::starfox_ex
                && game.flow_state()
                    == starfox::simulation::GameFlowState::title
                && !ex_title_logo_screen;
            if (!boss_roll) {
                ppu.bg1_scroll_x = interpolate_raster_word(
                    previous_raster_motion.bg1_scroll_x,
                    current_raster_motion.bg1_scroll_x);
                ppu.bg1_scroll_y = interpolate_raster_word(
                    previous_raster_motion.bg1_scroll_y,
                    current_raster_motion.bg1_scroll_y);
                ppu.bg3_scroll_x = interpolate_raster_word(
                    previous_raster_motion.bg3_scroll_x,
                    current_raster_motion.bg3_scroll_x);
                ppu.bg3_scroll_y = interpolate_raster_word(
                    previous_raster_motion.bg3_scroll_y,
                    current_raster_motion.bg3_scroll_y);
            }
            for (std::size_t line = 0;
                 line < ppu.bg2_horizontal_offsets.size(); ++line) {
                const auto previous_offset =
                    previous_raster_motion.bg2_horizontal_offsets[line];
                const auto current_offset =
                    current_raster_motion.bg2_horizontal_offsets[line];
                // Mode 2 horizontal-offset words are 13-bit SNES scroll
                // values.  Treating them as signed 16-bit coordinates made
                // an otherwise tiny wrap (8191 -> 0) interpolate through
                // thousands of pixels.  On the black-hole background that
                // appeared as one wildly displaced scanline/frame before the
                // next completed source update restored it.
                auto difference =
                    (static_cast<std::int32_t>(current_offset) -
                        static_cast<std::int32_t>(previous_offset))
                    & 0x1fff;
                if (difference > 4'095) difference -= 8'192;
                auto offset = (static_cast<std::int32_t>(previous_offset)
                        & 0x1fff)
                    + static_cast<std::int32_t>(std::lround(
                        static_cast<double>(difference)
                        * interpolation_alpha));
                offset %= 8'192;
                if (offset < 0) offset += 8'192;
                ppu.bg2_horizontal_offsets[line] =
                    static_cast<std::int16_t>(offset);
            }
            for (std::size_t index = 0;
                 index < current_raster_motion.bg2_vertical_offsets.size();
                 ++index) {
                const auto previous_value =
                    previous_raster_motion.bg2_vertical_offsets[index];
                const auto current_value =
                    current_raster_motion.bg2_vertical_offsets[index];
                auto interpolated = current_value;
                // DOVOFS words contain a 13-bit wrapping scroll value and a
                // validity flag. Interpolate only while the same table entry
                // remains active, following the shortest wrapped distance.
                if (((previous_value ^ current_value) & 0x4000U) == 0U
                    && (current_value & 0x4000U) != 0U) {
                    const auto previous_scroll =
                        static_cast<std::int32_t>(previous_value & 0x1fffU);
                    const auto current_scroll =
                        static_cast<std::int32_t>(current_value & 0x1fffU);
                    // These entries include CALCBGSCROLL's 9-bit phase.
                    // 511 -> 0 is one pixel, not a half-screen excursion.
                    const auto scroll = starfox::timing::interpolate_wrapped_scroll(
                        previous_scroll, current_scroll, interpolation_alpha, 0x1ffU);
                    interpolated = static_cast<std::uint16_t>(
                        (current_value & 0xe000U)
                        | static_cast<std::uint16_t>(scroll));
                }
                const auto byte = (0x2fa0U + index) * 2U;
                ppu.vram[byte] = static_cast<std::uint8_t>(interpolated);
                ppu.vram[byte + 1U] =
                    static_cast<std::uint8_t>(interpolated >> 8U);
            }
            auto circle = interpolate_circle_effect(
                previous_circle, current_circle, interpolation_alpha);
            circle.centre_x = static_cast<std::int16_t>(
                circle.centre_x + viewport_origin);
            auto planet_presentation = game.planet_presentation_state();
            framebuffer.end_write_coverage();
            framebuffer.clear(0U);
            window.begin_temporal_frame(game.scene_revision(),
                std::uint32_t(game.experience()) | (std::uint32_t(game.flow_state())<<8)
                | (std::uint32_t(game.in_setup_menu())<<24) | (std::uint32_t(game.stereo_output())<<25));
            superfx_frame.clear(0U);
            superfx_surfaces.clear();
            const bool record_raster=(std::getenv("STARFOX_TEST_GPU_RASTER") || window.native_gpu_enabled())
                && game.renderer_mode()==starfox::simulation::RendererMode::gpu && !gpu_raster_failed;
            bool resident_raster=false;
            std::unique_ptr<DeferredBackground> deferred_background;
            std::unique_ptr<DeferredBackground> late_cartridge;
            std::vector<std::uint8_t> background_cpu_coverage;
            std::optional<starfox::render::Framebuffer> temporal_background;
            const bool record_models=record_raster && window.native_gpu_enabled()
                && (std::getenv("STARFOX_DISABLE_GPU_GEOMETRY")==nullptr || game.stereo_output()!=0U);
            if(record_models) recorded_scene.reset(superfx_frame.stored_width(),superfx_frame.stored_height());
            bool ray_scene_complete=true;
            controls_model_draws.clear();
            const auto draw_model=[&](const starfox::assets::Shape& shape,
                const starfox::render::RenderPose& pose,starfox::render::Framebuffer& target,
                bool clear_target=false,starfox::render::SurfaceBuffer* surfaces=nullptr,
                starfox::render::shadows::Scene* shadows=nullptr,
                std::optional<starfox::render::GpuModelIdentity> identity=std::nullopt) {
                if(test_frames && presented_frames+1U==test_frames
                    && std::getenv("STARFOX_TRACE_FINAL_MODEL_POSES") && &target==&superfx_frame) {
                    const auto old_precision=std::cerr.precision(17);
                    std::cerr<<"final-model-pose: header="<<shape.header.address
                        <<" colour="<<shape.header.colour_pointer<<" name="<<shape.name
                        <<" xyz="<<pose.x<<','<<pose.y<<','<<pose.z
                        <<" angles="<<pose.pitch<<','<<pose.yaw<<','<<pose.roll
                        <<" scale="<<pose.scale<<" vanish="<<pose.vanish_x<<','<<pose.vanish_y
                        <<" frame="<<pose.animation_frame<<" colour-frame="<<pose.colour_frame
                        <<" matrix="<<pose.use_rotation_matrix<<" continuous="<<pose.continuous_geometry
                        <<" subpixel="<<pose.subpixel_projection<<" force="<<pose.force_colour
                        <<" depth="<<pose.source_depth<<" rotation=";
                    for(const auto value:pose.rotation_matrix) std::cerr<<value<<',';
                    std::cerr<<" lighting="<<pose.use_source_lighting_state<<':';
                    for(const auto value:pose.source_lighting_matrix) std::cerr<<value<<',';
                    std::cerr<<'\n';std::cerr.precision(old_precision);
                }
                if(record_models && (&target==&superfx_frame || (controls_scene && &target==&controls_player_layer)) && !clear_target) {
                    starfox::render::GpuModelDraw draw{&shape,pose,render_settings,surfaces!=nullptr,identity,false,
                        shadows!=nullptr && !pose.simple_scaled_sprite
                            && std::any_of(shape.faces.begin(),shape.faces.end(),[](const auto& face){return !face.sprite && face.vertex_indices.size()>=3;})};
                    // The source Controls player is a final model layer above
                    // its demonstration effects. Both layers use the same
                    // dimensions, +16 Y offset and flight-panel clip rectangle.
                    if(&target==&controls_player_layer) controls_model_draws.push_back(draw);
                    else recorded_scene.append_model(raster_commands,draw);
                } else {
                    if(shadows && !pose.simple_scaled_sprite) ray_scene_complete=false;
                    renderer.draw(shape,pose,target,clear_target,surfaces,shadows);
                }
            };
            starfox::render::LayerCompositeSettings resident_layer;
            if(record_raster) {
                raster_commands.reset(superfx_frame.stored_width(),superfx_frame.stored_height());
                superfx_frame.record_to(&raster_commands);
            }
            shadow_scene.clear();
            shadow_mask.clear();
            for(auto& mask:stereo_shadow_masks) mask.clear();
            bool resident_shadow=false;
            bool mono_shadows_deferred=false,cpu_casters_collected=false;
            std::array<bool,2> stereo_resident_shadow{};
            superfx_ui.clear(0U);
            superfx_hud.clear(0U);
            comms_hud.clear(0U);
            if (controls_scene) controls_player_layer.clear(0U);
            if (present_native_ex_bitmap) {
                native_ex_overlay.clear(0U);
            }
            if (planet_presentation.briefing_layers) {
                planet_overlay.clear(0U);
                planet_text_overlay.clear(0U);
            }
            // EX's 224-pixel Super FX bitmap is centred inside a 256-pixel
            // BG1 surface with 16-pixel black guard columns. Retain those at
            // source 4:3, but omit them when the surrounding BG2 is expanded.
            const auto native_menu_guard_inset =
                (ex_native_menu || extend_ex_title_art)
                    && display_width > snes_width ? 16U : 0U;
            planet_presentation.isolate_left = static_cast<std::int16_t>(
                planet_presentation.isolate_left + viewport_origin);
            planet_presentation.isolate_right = static_cast<std::int16_t>(
                planet_presentation.isolate_right + viewport_origin);
            const bool frontend_margin_fill=viewport_origin>0 && (controls_scene
                || game.flow_state()==starfox::simulation::GameFlowState::game_over
                || game.flow_state()==starfox::simulation::GameFlowState::continue_choice);
            // Isolated briefing BG2/text target their own framebuffers. The
            // recorder's target check keeps those separate while the main
            // BG1/OBJ scene can remain GPU resident.
            const bool record_background=record_models
                && !std::getenv("STARFOX_DISABLE_GPU_BACKGROUND")
                && !std::getenv("STARFOX_CAPTURE_INDEXED_PATH");
            if(record_background) {
                deferred_background=std::make_unique<DeferredBackground>();
                deferred_background->margin_origin=frontend_margin_fill || (ex_title_logo_screen && viewport_origin>0)?unsigned(viewport_origin):0;
                deferred_background->repair_margins=ex_title_logo_screen;
                deferred_background->ppu=std::make_shared<const starfox::simulation::SnesPpuState>(ppu);
                const bool world=game.flow_state()==starfox::simulation::GameFlowState::gameplay
                    || game.flow_state()==starfox::simulation::GameFlowState::training
                    || game.flow_state()==starfox::simulation::GameFlowState::intro;
                deferred_background->tag=world?starfox::render::PixelLayer::background:starfox::render::PixelLayer::two_d;
                deferred_background->base_tag=world?starfox::render::PixelLayer::background:starfox::render::PixelLayer::three_d;
                deferred_background->scene.reset(framebuffer.stored_width(),framebuffer.stored_height());
                deferred_background->pending.reset(framebuffer.stored_width(),framebuffer.stored_height());
                background_renderer.recording=deferred_background.get();background_renderer.target=&framebuffer;
                framebuffer.record_to(&deferred_background->pending);
            }
            // Tile/sprite presentation is still vastly oversampled at the
            // 360/480 Hz output choices. Sample that cartridge layer at a
            // smooth 180/160 Hz while the interpolated Super FX world, HUD,
            // cursor, and window effects continue at the requested rate.
            const auto presentation_background_cadence =
                static_cast<std::uint16_t>(
                    game.presentation_fps() <= 240U
                        ? 1U
                        : (game.presentation_fps() + 179U) / 180U);
            const auto cache_complete_cartridge_layer =
                ppu.background_mode == 1U
                || (ppu.background_mode == 2U && !gameplay_hud);
            const auto reuse_complete_cartridge_layer =
                cache_complete_cartridge_layer && !record_background
                && presentation_background_cadence > 1U
                && presented_frames % presentation_background_cadence != 0U
                && cartridge_layer_valid
                && cartridge_layer_cache.width() == framebuffer.width()
                && cartridge_layer_cache.height() == framebuffer.height()
                && cartridge_layer_cache.draw_scale()
                    == framebuffer.draw_scale()
                && cartridge_layer_cache.pixels().size()
                    == framebuffer.pixels().size()
                && cartridge_layer_scene_revision == game.scene_revision()
                && cartridge_layer_background_id == game.map().background()
                && cartridge_layer_background_mode == ppu.background_mode
                && cartridge_layer_flow_state == static_cast<std::uint8_t>(
                    game.flow_state());
            if (reuse_complete_cartridge_layer) {
                framebuffer.copy_pixels_from(cartridge_layer_cache);
                ++cartridge_layer_temporal_hits;
            } else if (ppu.background_mode == 1U) {
                ++profiled_background_modes[1U];
                const auto native_menu_bg1 = game.flow_state()
                    == starfox::simulation::GameFlowState::ex_pregame_menu;
                const auto extend_title_backdrop = display_width > snes_width
                    && game.flow_state()
                        == starfox::simulation::GameFlowState::title
                    && game.experience()==starfox::simulation::Experience::starfox_ex;
                // Original BG3 includes regional logo ink even in its low
                // pass. Repeating it wraps a detached logo column into the
                // outer margin. Its world stars already extend separately.
                // The EX title's low-priority BG3 cells are its sparse native
                // star/backdrop layer. Repeat only that pass through wide
                // margins so the moving Super FX ship never enters a solid
                // 4:3 side band. High BG3 (PRESS START) and BG2's logo/roster
                // remain centred and are restored in their source priority
                // order after the model pass below.
                background_renderer.draw_bg3(
                    ppu, framebuffer, starfox::render::TilePriorityPass::low,
                    viewport_origin,
                    extend_cartridge_scene || extend_title_backdrop);
                sprite_renderer.draw_objects(ppu, framebuffer, 0U, viewport_origin,
                    extend_cartridge_scene, anchor_edge_hud, gameplay_layout,
                    suppress_configurable_hud && gameplay_hud);
                if (!ppu.bg3_high_priority) {
                    background_renderer.draw_bg3(
                        ppu, framebuffer, starfox::render::TilePriorityPass::high,
                        viewport_origin, extend_cartridge_scene);
                }
                sprite_renderer.draw_objects(ppu, framebuffer, 1U, viewport_origin,
                    extend_cartridge_scene, anchor_edge_hud, gameplay_layout,
                    suppress_configurable_hud && gameplay_hud);
                background_renderer.draw_bg2(ppu, background_x, background_y,
                    framebuffer, starfox::render::TilePriorityPass::low,
                    viewport_origin,
                    extend_cartridge_scene || extend_ex_title_art,
                    !extend_ex_title_art);
                if (native_menu_bg1) {
                    // CONTINUE.ASM uses Mode 1 for its first seven random
                    // backdrops and keeps the source menu text in BG1. The
                    // host normally replaces BG1 with 3D geometry, so expose
                    // this cartridge bitmap only for EX's native menu and
                    // keep it confined to the original 256-pixel canvas.
                    background_renderer.draw_bg1(ppu, framebuffer,
                        starfox::render::TilePriorityPass::low,
                        viewport_origin, false, native_menu_guard_inset);
                }
                sprite_renderer.draw_objects(ppu, framebuffer, 2U, viewport_origin,
                    extend_cartridge_scene, anchor_edge_hud, gameplay_layout,
                    suppress_configurable_hud && gameplay_hud);
                background_renderer.draw_bg2(ppu, background_x, background_y,
                    framebuffer, starfox::render::TilePriorityPass::high,
                    viewport_origin,
                    extend_cartridge_scene || extend_ex_title_art,
                    !extend_ex_title_art);
                if (native_menu_bg1) {
                    background_renderer.draw_bg1(ppu, framebuffer,
                        starfox::render::TilePriorityPass::high,
                        viewport_origin, false, native_menu_guard_inset);
                }
            } else if (ppu.background_mode == 2U) {
                ++profiled_background_modes[2U];
                if (gameplay_hud) ++profiled_gameplay_hud_frames;
                const auto native_mode2_bg1 = game.flow_state()
                    == starfox::simulation::GameFlowState::ex_pregame_menu;
                if (gameplay_hud) {
                    if(record_background) {
                        starfox::render::GpuBackgroundSettings settings;
                        settings.layer=2;settings.scroll_x=background_x;settings.scroll_y=background_y;
                        settings.horizontal_origin=viewport_origin;settings.extend_horizontal=extend_cartridge_scene;
                        if(std::getenv("STARFOX_TEST_DLSS_EVALUATE")) {
                            settings.terrain_source_rows=starfox::render::authored_terrain_rows(ppu);
                            if(test_frames && presented_frames+1==test_frames && std::getenv("STARFOX_TRACE_GPU"))
                                std::cerr<<"terrain-profile: rows="<<settings.terrain_source_rows[0]<<':'<<settings.terrain_source_rows[1]
                                    <<" hash="<<starfox::render::terrain_tilemap_hash(ppu)<<'\n';
                        }
                        settings.single_occurrence_top_rows=game.map().background()==space_planet_background?168U:
                            ((macbeth_approach_background && game.map().background()==macbeth_approach_background)
                            || (macbeth_departure_background && game.map().background()==macbeth_departure_background))?framebuffer.height():0U;
                        if(ex_twin_planet_background && game.map().background()==ex_twin_planet_background)
                            settings.unique_regions.assign(std::begin(ex_twin_planets),std::end(ex_twin_planets));
                        else if((ex_face_planet_background && game.map().background()==ex_face_planet_background)
                            || (dimension_background && game.map().background()==dimension_background))
                            settings.unique_regions.assign(std::begin(ex_face_planets),std::end(ex_face_planets));
                        background_renderer.record(framebuffer,std::move(settings));
                    } else {
                    // Gameplay's complete OBJ HUD is intentionally restored
                    // after the Super FX world below. Rendering BG2 twice and
                    // three disposable OAM priority passes here therefore did
                    // no visible work. Combine the two tile priorities in one
                    // traversal; this is the dominant wide/high-FPS path.
                    std::array<std::uint16_t, 32U> vertical_key{};
                    for (std::size_t index = 0U;
                         index < vertical_key.size(); ++index) {
                        const auto byte = (0x2fa0U + index) * 2U;
                        vertical_key[index] = static_cast<std::uint16_t>(
                            ppu.vram[byte])
                            | (static_cast<std::uint16_t>(
                                ppu.vram[byte + 1U]) << 8U);
                    }
                    const auto structural_match = mode2_background_valid
                        && mode2_background_cache.width()
                            == framebuffer.width()
                        && mode2_background_cache.height()
                            == framebuffer.height()
                        && mode2_background_cache.draw_scale()
                            == framebuffer.draw_scale()
                        && mode2_background_cache.pixels().size()
                            == framebuffer.pixels().size()
                        && mode2_background_scene_revision
                            == game.scene_revision()
                        && mode2_background_id == game.map().background()
                        && mode2_background_ppu.background_mode
                            == ppu.background_mode
                        && mode2_background_ppu.bg2_tile_size_16
                            == ppu.bg2_tile_size_16
                        && mode2_background_ppu.mosaic == ppu.mosaic
                        && mode2_background_ppu.bg2_character_base
                            == ppu.bg2_character_base
                        && mode2_background_ppu.bg2_screen_base
                            == ppu.bg2_screen_base
                        && mode2_background_ppu.bg2_screen_size
                            == ppu.bg2_screen_size
                        // Widescreen blank fills choose a dark CGRAM entry;
                        // that cached choice is invalid after a palette load.
                        && mode2_background_ppu.cgram == ppu.cgram
                        && mode2_background_ppu.main_screen == ppu.main_screen
                        && mode2_background_ppu.bg2_vertical_offsets_enabled
                            == ppu.bg2_vertical_offsets_enabled
                        && mode2_background_ppu.bg2_horizontal_offsets_enabled
                            == ppu.bg2_horizontal_offsets_enabled
                        && mode2_background_ppu.bg2_scanline_scroll_enabled
                            == ppu.bg2_scanline_scroll_enabled
                        && mode2_background_ppu.tunnel_scene == ppu.tunnel_scene;
                    // At the two extreme presentation rates the source
                    // raster does not need to be rebuilt hundreds of times
                    // per second: 360 Hz samples it at 180 Hz and 480 Hz at
                    // 160 Hz. Models, HUD, the crosshair, and presentation
                    // wipes still update at the requested refresh rate.
                    const auto temporal_reuse = structural_match
                        && presented_frames % presentation_background_cadence
                            != 0U;
                    const auto same_background = temporal_reuse
                        || (structural_match
                        // VRAM can change only at a completed source update.
                        // This invalidates animated/reloaded source graphics
                        // without comparing the live Super FX bitmap, whose
                        // unrelated BG1 writes previously defeated the cache.
                        && mode2_background_source_frame == source_logic_frames
                        && mode2_background_x == background_x
                        && mode2_background_y == background_y
                        && mode2_background_vertical == vertical_key
                        && mode2_background_ppu.bg2_horizontal_offsets
                            == ppu.bg2_horizontal_offsets
                        && mode2_background_ppu.bg2_scanline_scroll_y
                            == ppu.bg2_scanline_scroll_y);
                    if (same_background) {
                        if (temporal_reuse) {
                            ++mode2_background_temporal_hits;
                        } else {
                            ++mode2_background_exact_hits;
                        }
                        framebuffer.copy_pixels_from(mode2_background_cache);
                    } else {
                        ++mode2_background_misses;
                        background_renderer.draw_bg2(ppu, background_x,
                            background_y, framebuffer,
                            starfox::render::TilePriorityPass::all,
                            viewport_origin, extend_cartridge_scene, true,
                            false,
                            game.map().background()
                                    == space_planet_background
                                ? 168U
                                : ((macbeth_approach_background != 0U
                                        && game.map().background() == macbeth_approach_background)
                                    || (macbeth_departure_background != 0U
                                        && game.map().background() == macbeth_departure_background))
                                    ? framebuffer.height() : 0U,
                            ex_twin_planet_background != 0U
                                && game.map().background() == ex_twin_planet_background
                                ? std::span<const starfox::render::BackgroundUniqueRegion>{ex_twin_planets}
                                : ((ex_face_planet_background != 0U
                                    && game.map().background() == ex_face_planet_background)
                                    || (dimension_background != 0U
                                        && game.map().background() == dimension_background))
                                ? std::span<const starfox::render::BackgroundUniqueRegion>{ex_face_planets}
                                : std::span<const starfox::render::BackgroundUniqueRegion>{});
                        mode2_background_cache.set_draw_scale(
                            framebuffer.draw_scale());
                        mode2_background_cache.resize(
                            framebuffer.width(), framebuffer.height());
                        mode2_background_cache.copy_pixels_from(framebuffer);
                        mode2_background_ppu = ppu;
                        mode2_background_vertical = vertical_key;
                        mode2_background_x = background_x;
                        mode2_background_y = background_y;
                        mode2_background_source_frame = source_logic_frames;
                        mode2_background_scene_revision =
                            game.scene_revision();
                        mode2_background_id = game.map().background();
                        mode2_background_valid = true;
                    }
                    }
                } else {
                    background_renderer.draw_bg2(ppu, background_x, background_y,
                        framebuffer, starfox::render::TilePriorityPass::low,
                        viewport_origin, extend_cartridge_scene);
                    sprite_renderer.draw_objects(ppu, framebuffer, 0U,
                        viewport_origin, extend_cartridge_scene,
                        anchor_edge_hud, gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                    if (native_mode2_bg1) {
                        background_renderer.draw_bg1(ppu, framebuffer,
                            starfox::render::TilePriorityPass::low,
                            viewport_origin, false, native_menu_guard_inset);
                    }
                    sprite_renderer.draw_objects(ppu, framebuffer, 1U,
                        viewport_origin, extend_cartridge_scene,
                        anchor_edge_hud, gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                    background_renderer.draw_bg2(ppu, background_x, background_y,
                        framebuffer, starfox::render::TilePriorityPass::high,
                        viewport_origin, extend_cartridge_scene);
                    sprite_renderer.draw_objects(ppu, framebuffer, 2U,
                        viewport_origin, extend_cartridge_scene,
                        anchor_edge_hud, gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                    if (native_mode2_bg1) {
                        background_renderer.draw_bg1(ppu, framebuffer,
                            starfox::render::TilePriorityPass::high,
                            viewport_origin, false, native_menu_guard_inset);
                    }
                }
            } else if (ppu.background_mode == 3U) {
                ++profiled_background_modes[3U];
                auto& bg2_target = planet_presentation.briefing_layers
                    ? planet_overlay : framebuffer;
                if ((ppu.main_screen & 0x02U) != 0U) {
                    background_renderer.draw_bg2(ppu, background_x, background_y,
                        bg2_target, starfox::render::TilePriorityPass::low,
                        viewport_origin, extend_cartridge_scene);
                }
                if ((ppu.main_screen & 0x10U) != 0U) {
                    sprite_renderer.draw_objects(
                        ppu, framebuffer, 0U, viewport_origin,
                        extend_cartridge_scene, anchor_edge_hud, gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                }
                if ((ppu.main_screen & 0x01U) != 0U) {
                    background_renderer.draw_bg1(
                        ppu, framebuffer, starfox::render::TilePriorityPass::low,
                        viewport_origin, extend_cartridge_scene);
                }
                if ((ppu.main_screen & 0x10U) != 0U) {
                    sprite_renderer.draw_objects(
                        ppu, framebuffer, 1U, viewport_origin,
                        extend_cartridge_scene, anchor_edge_hud, gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                }
                if ((ppu.main_screen & 0x02U) != 0U) {
                    background_renderer.draw_bg2(ppu, background_x, background_y,
                        bg2_target, starfox::render::TilePriorityPass::high,
                        viewport_origin, extend_cartridge_scene);
                }
                if ((ppu.main_screen & 0x10U) != 0U) {
                    sprite_renderer.draw_objects(
                        ppu, framebuffer, 2U, viewport_origin,
                        extend_cartridge_scene, anchor_edge_hud, gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                }
                if ((ppu.main_screen & 0x01U) != 0U) {
                    background_renderer.draw_bg1(
                        ppu, framebuffer, starfox::render::TilePriorityPass::high,
                        viewport_origin, extend_cartridge_scene);
                }
                if ((ppu.main_screen & 0x10U) != 0U) {
                    // PLANETS places the four-piece map Arwing at OBJ
                    // priority 3 so it stays above every planet and route
                    // layer. Omitting the final priority pass discarded the
                    // ship even though its OAM and character data were valid.
                    sprite_renderer.draw_objects(
                        ppu, framebuffer, 3U, viewport_origin,
                        extend_cartridge_scene, anchor_edge_hud,
                        gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                }
            } else {
                ++profiled_background_modes[std::min<std::size_t>(
                    ppu.background_mode, profiled_background_modes.size() - 1U)];
                background_renderer.draw_bg2(
                    ppu, background_x, background_y, framebuffer,
                    starfox::render::TilePriorityPass::all, viewport_origin,
                    extend_cartridge_scene);
                background_renderer.draw_bg3(ppu, framebuffer,
                    starfox::render::TilePriorityPass::all, viewport_origin,
                    extend_cartridge_scene);
                for (std::uint8_t priority = 0; priority < 3U; ++priority) {
                    sprite_renderer.draw_objects(
                        ppu, framebuffer, priority, viewport_origin,
                        extend_cartridge_scene, anchor_edge_hud, gameplay_layout,
                        suppress_configurable_hud && gameplay_hud);
                }
            }
            if(record_background) {
                framebuffer.record_to(nullptr);
                deferred_background->scene.finish(deferred_background->pending);
                if(deferred_background->tag==starfox::render::PixelLayer::background) {
                    for(const auto& draw:deferred_background->scene.draws())
                        if(const auto* raster=std::get_if<starfox::render::GpuRasterDraw>(&draw))
                            for(auto& command:raster->commands->commands) command.tag=unsigned(starfox::render::PixelLayer::background);
                }
                background_renderer.recording=nullptr;background_renderer.target=nullptr;
                framebuffer.begin_write_coverage();
            }
            if (ex_title_logo_screen && viewport_origin > 0 && !deferred_background) {
                // TITLEI uses a black BG2 tile while CGRAM colour zero is the
                // brown Macbeth backdrop. The original 256-pixel canvas never
                // exposes colour zero, but a wide host framebuffer otherwise
                // turns untouched margin pixels brown. Replace only those
                // transparent pixels with the source tile's indexed black;
                // the extended low-priority title stars must survive instead
                // of being wiped back into solid side bands.
                const auto backdrop = framebuffer.get(
                    static_cast<std::uint32_t>(viewport_origin), 0U);
                const auto right = viewport_origin
                    + static_cast<std::int32_t>(snes_width);
                for (std::int32_t y = 0;
                     y < static_cast<std::int32_t>(framebuffer.height()); ++y) {
                    for (std::int32_t x = 0; x < viewport_origin; ++x) {
                        if (framebuffer.get(x, y) == 0U) {
                            framebuffer.set(x, y, backdrop);
                        }
                    }
                    for (std::int32_t x = right;
                         x < static_cast<std::int32_t>(framebuffer.width()); ++x) {
                        if (framebuffer.get(x, y) == 0U) {
                            framebuffer.set(x, y, backdrop);
                        }
                    }
                }
            }
            if (cache_complete_cartridge_layer
                && !reuse_complete_cartridge_layer && !record_background) {
                cartridge_layer_cache.set_draw_scale(
                    framebuffer.draw_scale());
                cartridge_layer_cache.resize(
                    framebuffer.width(), framebuffer.height());
                cartridge_layer_cache.copy_pixels_from(framebuffer);
                cartridge_layer_scene_revision = game.scene_revision();
                cartridge_layer_background_id = game.map().background();
                cartridge_layer_background_mode = ppu.background_mode;
                cartridge_layer_flow_state = static_cast<std::uint8_t>(
                    game.flow_state());
                cartridge_layer_valid = true;
                ++cartridge_layer_misses;
            }
            const auto profile_background_done =
                std::chrono::steady_clock::now();
            // This pass contains scenery only. Mark its cartridge pixels
            // before compositing models, dialogue and HUD; cached backgrounds
            // take this path too. Keep front-end/native menu artwork protected.
            const auto world_background = game.flow_state() == starfox::simulation::GameFlowState::gameplay
                || game.flow_state() == starfox::simulation::GameFlowState::training
                || game.flow_state() == starfox::simulation::GameFlowState::intro;
            // Layer identity must not depend on which effect happens to be
            // enabled. In particular, ray tracing alone needs the ground to
            // receive shadows rather than being mistaken for protected HUD.
            if (world_background) {
                for (auto& tag : framebuffer.layer_tags()) {
                    tag = static_cast<std::uint8_t>(starfox::render::PixelLayer::background);
                }
            }
            struct VisibleObject {
                starfox::simulation::ObjectHandle handle{};
                starfox::timing::RenderTransform transform;
                CameraPoint position;
                double source_depth{};
                starfox::simulation::MatrixQ15 object_matrix{};
                starfox::simulation::MatrixQ15 source_object_matrix{};
            };
            std::vector<VisibleObject> visible;
            auto camera = starfox::timing::interpolate(
                previous_camera, current_camera, interpolation_alpha);
            const auto camera_matrix_at = [&](const auto& snapshot,
                                               bool apply_mouse_offsets) {
                return starfox::simulation::rotation_matrix_q15(
                    trigonometry,
                    static_cast<std::int16_t>(static_cast<std::uint16_t>(
                        static_cast<double>(snapshot.pitch)
                            + (mouse_camera_scene && apply_mouse_offsets
                                ? mouse_camera.pitch_offset : 0.0))),
                    static_cast<std::int16_t>(static_cast<std::uint16_t>(
                        static_cast<double>(snapshot.yaw)
                            + (mouse_camera_scene && apply_mouse_offsets
                                ? mouse_camera.yaw_offset : 0.0))),
                    static_cast<std::int16_t>(snapshot.roll));
            };
            // Interpolate complete orthonormal transforms at the selected
            // headset/output cadence. Euler interpolation can accelerate or
            // kink compound rotations even though the fixed simulation clock
            // is correct; normalized matrix interpolation changes only the
            // presentation between the same two 20 Hz source states.
            const auto base_view_matrix =
                starfox::simulation::interpolate_rotation_matrix_q15(
                    camera_matrix_at(previous_camera, false),
                    camera_matrix_at(current_camera, false), interpolation_alpha);
            const auto view_matrix =
                starfox::simulation::interpolate_rotation_matrix_q15(
                    camera_matrix_at(previous_camera, true),
                    camera_matrix_at(current_camera, true), interpolation_alpha);
            if (mouse_camera_scene) {
                camera.pitch += mouse_camera.pitch_offset;
                camera.yaw += mouse_camera.yaw_offset;
                const auto current_player = current.find(game.player());
                if (current_player != current.end()) {
                    auto player_transform = starfox::timing::interpolate(
                        current_player->second.transform,
                        current_player->second.transform, 1.0);
                    const auto previous_player = previous.find(game.player());
                    if (previous_player != previous.end()) {
                        player_transform = starfox::timing::interpolate(
                            previous_player->second.transform,
                            current_player->second.transform,
                            interpolation_alpha);
                    }
                    constexpr double q15 = 32'768.0;
                    const std::array<double, 3> offset{
                        camera.x - player_transform.x,
                        camera.y - player_transform.y,
                        camera.z - player_transform.z,
                    };
                    // Express the original camera/player arm in the source
                    // camera basis, then rebuild it in the mouse-adjusted
                    // basis. This orbits around the Arwing instead of turning
                    // in place around the camera's own origin.
                    const std::array<double, 3> local{
                        (offset[0] * base_view_matrix[0]
                            + offset[1] * base_view_matrix[3]
                            + offset[2] * base_view_matrix[6]) / q15,
                        (offset[0] * base_view_matrix[1]
                            + offset[1] * base_view_matrix[4]
                            + offset[2] * base_view_matrix[7]) / q15,
                        (offset[0] * base_view_matrix[2]
                            + offset[1] * base_view_matrix[5]
                            + offset[2] * base_view_matrix[8]) / q15,
                    };
                    camera.x = player_transform.x
                        + (local[0] * view_matrix[0]
                            + local[1] * view_matrix[1]
                            + local[2] * view_matrix[2]) / q15;
                    camera.y = player_transform.y
                        + (local[0] * view_matrix[3]
                            + local[1] * view_matrix[4]
                            + local[2] * view_matrix[5]) / q15;
                    camera.z = player_transform.z
                        + (local[0] * view_matrix[6]
                            + local[1] * view_matrix[7]
                            + local[2] * view_matrix[8]) / q15;
                }
            }
            if (mouse_camera_scene && mouse_camera.zoom_offset != 0.0) {
                constexpr double q15 = 32'768.0;
                // The third transform column is the adjusted camera's world-
                // space forward axis. Moving opposite it increases distance;
                // wheel-up decreases the offset and therefore zooms inward.
                camera.x -= static_cast<double>(view_matrix[2]) / q15
                    * mouse_camera.zoom_offset;
                camera.y -= static_cast<double>(view_matrix[5]) / q15
                    * mouse_camera.zoom_offset;
                camera.z -= static_cast<double>(view_matrix[8]) / q15
                    * mouse_camera.zoom_offset;
            }
            const starfox::timing::RenderTransform source_camera{
                static_cast<double>(current_camera.x),
                static_cast<double>(current_camera.y),
                static_cast<double>(current_camera.z),
                static_cast<double>(current_camera.pitch),
                static_cast<double>(current_camera.yaw),
                static_cast<double>(current_camera.roll)};
            const auto source_view_matrix = starfox::simulation::rotation_matrix_q15(
                trigonometry,
                static_cast<std::int16_t>(current_camera.pitch),
                static_cast<std::int16_t>(current_camera.yaw),
                static_cast<std::int16_t>(current_camera.roll));
            text_renderer.set_language(game.language());
            const auto planet_screen = game.flow_state()
                == starfox::simulation::GameFlowState::planet_select
                || game.flow_state()
                == starfox::simulation::GameFlowState::planet_travel
                || game.flow_state()
                == starfox::simulation::GameFlowState::continue_choice;
            const auto controls_screen = game.flow_state()
                    == starfox::simulation::GameFlowState::controls_type
                || game.flow_state()
                    == starfox::simulation::GameFlowState::controls_choice;
            if (!planet_screen && game.map().dots_mode() < 0) {
                const auto dust_offset_x=controls_screen ? static_cast<int>(game.map().read_native_word(vanish_x_address))
                        + superfx_ui_offset_x - static_cast<int>(superfx_frame.width() / 2U) : 0;
                const auto dust_offset_y=controls_screen ? static_cast<int>(game.map().read_native_word(vanish_y_address))
                        + (extend_scene_vertical ? superfx_offset_y : 0)
                        - static_cast<int>(superfx_frame.height() / 2U) : 0;
                if(record_models) {
                    auto dust=dust_renderer.prepare_dust(game.dust(),game.dust_point_count(),camera,view_matrix);
                    dust.offset_x=dust_offset_x;dust.offset_y=dust_offset_y;
                    recorded_scene.append_dust(raster_commands,{std::move(dust),superfx_frame.draw_scale()});
                    if(std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"GPU dust recorded\n";
                } else dust_renderer.draw(game.dust(),game.dust_point_count(),camera,view_matrix,
                    superfx_frame,dust_offset_x,dust_offset_y);
            } else if (!planet_screen && game.map().dots_mode() > 0) {
                if (grid_lines_address != 0U
                    && (game.map().read_native_word(grid_lines_address) != 0U
                        || (test_frames && std::getenv("STARFOX_TEST_GRID_LINES")))) {
                    if(record_models) {
                        const auto lines=dust_renderer.prepare_grid_lines(camera,view_matrix,
                            source_logic_frames,superfx_frame.width(),superfx_frame.height());
                        starfox::render::GpuGridDraw draw{camera,view_matrix,superfx_frame.draw_scale()};
                        draw.lines=true;draw.line_start=lines.start;
                        recorded_scene.append_grid(raster_commands,draw);
                        if(std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"GPU connected grid recorded\n";
                    } else dust_renderer.draw_grid_lines(camera, view_matrix,
                        source_logic_frames, superfx_frame);
                } else if(record_models) {
                    recorded_scene.append_grid(raster_commands,{camera,view_matrix,superfx_frame.draw_scale()});
                } else {
                    dust_renderer.draw_grid(camera, view_matrix, superfx_frame);
                }
            }
            for (const auto handle : game.draw_order()) {
                if (!game.objects().is_active(handle)) continue;
                const auto& object = game.objects().at(handle);
                const auto gameplay_crosshair_scene = game.flow_state()
                        == starfox::simulation::GameFlowState::gameplay
                    || game.flow_state()
                        == starfox::simulation::GameFlowState::training;
                // EX's "NEW" reticle is a short-lived 3D TEST_ISTRAT object,
                // not the four-OBJ retail reticle.  It can survive for a few
                // source updates after a map transition, so suppress it at
                // presentation time anywhere the gameplay HUD is inactive.
                if (!gameplay_crosshair_scene
                    && ex_crosshair_strategy_address != 0U
                    && object.strategy_address
                        == ex_crosshair_strategy_address) {
                    continue;
                }
                // invisible is sflag 27, stored in the fourth strategy byte.
                if ((object.strategy_flags[3] & 0x08U) != 0U) continue;
                const auto current_transform = current.find(handle);
                if (current_transform == current.end()) continue;
                auto prior = previous.find(handle);
                if (prior != previous.end()
                    && (prior->second.shape
                            != current_transform->second.shape
                        || prior->second.strategy_address
                            != current_transform->second.strategy_address
                        || prior->second.type
                            != current_transform->second.type
                        || prior->second.generation
                            != current_transform->second.generation)) {
                    // Object handles are cartridge slots, not stable entity
                    // IDs. A removed object can be replaced in the same slot
                    // between two source frames. Interpolating that new model
                    // from the old slot's pose made fresh controller/training
                    // ships appear off-screen and made multi-part bosses such
                    // as Linktron jump between unrelated component poses.
                    prior = previous.end();
                }
                // EX recycles particles through fixed sight-line stations.
                // Match station depth rather than interpolating a particle
                // as it advances from the near station to the far station.
                auto birth = current_transform->second;
                const starfox::render::ObjectPresentationSnapshot* sight_prior = nullptr;
                if (ex_crosshair_strategy_address != 0U
                    && object.strategy_address == ex_crosshair_strategy_address) {
                    sight_prior = starfox::render::reticle_previous_snapshot(
                        current_transform->second, current, previous, game.player());
                    prior = previous.end();
                    const auto owner = current.find(game.player());
                    const auto old_owner = previous.find(game.player());
                    if (owner != current.end() && old_owner != previous.end()) {
                        // A newly born piece has no prior entity. Anchor its
                        // first interpolated pose to the same player motion
                        // as the camera; holding it at the future world pose
                        // makes it kick sideways whenever the Arwing moves.
                        birth.transform = starfox::timing::relative_birth_snapshot(
                            birth.transform, old_owner->second.transform,
                            owner->second.transform);
                        birth.rotation_matrix = old_owner->second.rotation_matrix;
                    }
                }
                const auto& prior_snapshot = sight_prior ? *sight_prior
                    : prior == previous.end() ? birth : prior->second;
                auto render_previous=prior_snapshot;
                auto render_current=current_transform->second;
                if(flash_player_strategy_address!=0U
                    && object.strategy_address==flash_player_strategy_address) {
                    starfox::render::anchor_player_overlay(render_previous,render_current,
                        previous,current,game.player());
                }
                // TRAIL_ISTRAT pieces are discrete source afterimages. Moving
                // every clone through fractional positions made the Nintendo
                // logo look smeared after its main text had already settled.
                auto transform = starfox::timing::interpolate(
                    render_previous.transform,
                    render_current.transform,
                    object.strategy_address == trail_strategy_address
                        ? 1.0 : interpolation_alpha);
                if (ex_crosshair_strategy_address != 0U
                    && object.strategy_address == ex_crosshair_strategy_address) {
                    // Cancel decorative camera float for aiming markers only.
                    // The source VIEWPOSY = PVIEWPOSY + VIEWFLOATY keeps bobbing
                    // even while the player is held against a flight boundary.
                    transform.y += std::lerp(double(previous_view_float),
                        double(current_view_float), interpolation_alpha);
                }
                const auto transform_alpha =
                    object.strategy_address == trail_strategy_address
                        ? 1.0 : interpolation_alpha;
                const auto object_matrix =
                    starfox::render::interpolate_object_rotation(
                        render_previous, render_current,
                        transform_alpha, tunnel_arrow_gate_shape);
                const auto position = world_to_camera(
                    transform.x, transform.y, transform.z, camera, view_matrix);
                const auto source_position = world_to_camera(
                    render_current.transform.x,
                    render_current.transform.y,
                    render_current.transform.z,
                    source_camera, source_view_matrix);
                visible.push_back({handle, transform, position,
                    source_position.z, object_matrix,
                    render_current.rotation_matrix});
            }
            const auto game_frame = static_cast<std::uint8_t>(
                game.map().read_native_byte(game_frame_address) & 0x7fU);
            const auto depth_colours = game.map().read_native_word(
                depth_colours_address);
            const auto depth_thresholds = game.map().read_native_word(
                depth_thresholds_address);
            const auto display_frame = [game_frame](std::uint8_t object_frame) {
                return (object_frame & 0x80U) != 0U
                    ? static_cast<std::uint32_t>(object_frame & 0x7fU)
                    : static_cast<std::uint32_t>(game_frame);
            };
            const auto model_colour_override =
                game.model_colour_table_override();
            const auto effective_colour_table = [special_colour, red_colour,
                                                   white_colour,
                                                   model_colour_override](
                                                      const auto& object) {
                // MDRAWLIS.MC's -NAN modes 1-5 replace M_COLOURPTR before
                // hit-flash/special-colour handling, so the selected texture
                // table has priority for every object in the source list.
                if (model_colour_override) return *model_colour_override;
                const auto flags = object.strategy_flags[0];
                if ((flags & 0x40U) != 0U) return std::uint16_t{};
                if ((flags & 0x02U) != 0U && (flags & 0x20U) == 0U) {
                    return static_cast<std::uint16_t>(
                        (flags & 0x01U) != 0U ? red_colour : white_colour);
                }
                return static_cast<std::uint16_t>(
                    (flags & 0x01U) != 0U ? special_colour : object.colour_table);
            };
            // The simulation captured the enabled retail
            // marioshowview/mallrotzsort list at the 20 Hz source boundary.
            // Decode headers here only for visibility/LOD metadata; never
            // resort interpolated presentation coordinates.
            for (auto& item : visible) {
                const auto& object = game.objects().at(item.handle);
                // Text/trail objects use NULLSHAPE only as a strategy carrier;
                // TEXTURE_SCROLL_X, AL_COLTAB and AL_DEPTHOFFSET contain the
                // actual raster data. Requiring NULLSHAPE to decode before
                // this branch silently discarded Meteor's chained fire trail
                // whenever that placeholder was absent from the shape cache.
                if ((object.strategy_flags[0] & 0x40U) != 0U) continue;
                const auto colour_table = effective_colour_table(object);
                const auto base_shape_key = (static_cast<std::uint32_t>(object.shape) << 16U)
                    | colour_table;
                if (object.shape == 0 || invalid_shapes.contains(base_shape_key)) continue;
                auto base = shape_cache.find(base_shape_key);
                if (base == shape_cache.end()) {
                    try {
                        base = shape_cache.emplace(base_shape_key,
                            decoder.decode(object.shape, {}, colour_table)).first;
                    } catch (const std::exception&) {
                        invalid_shapes.insert(base_shape_key);
                        continue;
                    }
                }
            }
            std::erase_if(visible, [](const auto& item) { return item.handle == 0; });
            const auto shadows_enabled =
                (game.map().read_native_byte(player_fly_mode_address) & 0x08U) != 0U;
            const auto shadow_height = static_cast<std::int16_t>(
                game.map().read_native_word(shadow_height_address));
            const auto model_scale = static_cast<double>(
                game.model_scale_multiplier());
            const auto make_pose = [&](const VisibleObject& item, bool shadow) {
                const auto& object = game.objects().at(item.handle);
                const auto true_colour_shadow =
                    (object.strategy_flags[0] & 0x04U) != 0U;
                const auto position = shadow && !true_colour_shadow
                    ? world_to_camera(item.transform.x, shadow_height,
                        item.transform.z, camera, view_matrix)
                    : item.position;
                starfox::render::RenderPose pose;
                pose.x = position.x;
                pose.y = position.y;
                pose.z = position.z;
                pose.pitch = item.transform.pitch - camera.pitch;
                pose.yaw = item.transform.yaw - camera.yaw;
                pose.roll = item.transform.roll - camera.roll;
                pose.scale = model_scale;
                pose.vanish_x = static_cast<std::int16_t>(
                    game.map().read_native_word(vanish_x_address)
                    + superfx_ui_offset_x);
                pose.vanish_y = static_cast<std::int16_t>(
                    game.map().read_native_word(vanish_y_address)
                    + (extend_scene_vertical ? superfx_offset_y : 0));
                auto object_matrix = item.object_matrix;
                if (shadow) {
                    // mshowshadow clears rmat12/rmat22/rmat32 before the
                    // object matrix is composed with the view matrix.
                    object_matrix[1] = 0;
                    object_matrix[4] = 0;
                    object_matrix[7] = 0;
                    if (!true_colour_shadow) {
                        pose.force_colour = true;
                        pose.forced_colour = 0x09U;
                    }
                }
                const auto fractional_rotation = interpolation_alpha > 0.0
                    && interpolation_alpha < 1.0;
                pose.rotation_matrix = fractional_rotation
                    ? starfox::simulation::multiply_presentation_matrix_q15(
                        object_matrix, view_matrix)
                    : starfox::simulation::multiply_matrix_q15(
                        object_matrix, view_matrix);
                pose.use_rotation_matrix = true;
                auto source_object_matrix = item.source_object_matrix;
                if (shadow) {
                    source_object_matrix[1] = 0;
                    source_object_matrix[4] = 0;
                    source_object_matrix[7] = 0;
                }
                pose.source_depth = item.source_depth;
                pose.source_lighting_matrix =
                    starfox::simulation::multiply_matrix_q15(
                        source_object_matrix, source_view_matrix);
                pose.use_source_lighting_state = true;
                pose.continuous_geometry = game.presentation_fps() > 20U
                    && object.strategy_address != trail_strategy_address;
                pose.subpixel_projection = !game.paused()
                    && game.presentation_fps() > 20U
                    && interpolation_alpha > 0.0
                    && interpolation_alpha < 1.0
                    && object.strategy_address != trail_strategy_address;
                pose.animation_frame = display_frame(object.animation_frame);
                pose.colour_frame = display_frame(object.colour_frame);
                pose.texture_scroll_x = object.texture_scroll_x;
                pose.texture_scroll_y = object.texture_scroll_y;
                pose.wireframe_mode = wire_mode_address != 0U
                    ? game.map().read_native_byte(wire_mode_address) : 0U;
                pose.wobble_mode = wobble_mode_address != 0U
                    ? game.map().read_native_byte(wobble_mode_address) : 0U;
                pose.wave_mode = wave_mode_address != 0U
                    && game.map().read_native_byte(wave_mode_address) != 0U;
                pose.cel_mode = cel_mode_address != 0U
                    && game.map().read_native_byte(cel_mode_address) != 0U;
                pose.wave_offset = wave_offset_address != 0U
                    ? static_cast<std::int16_t>(game.map().read_native_word(
                        wave_offset_address)) : 0;
                pose.colour_warp = colour_warp_address != 0U
                    && game.map().read_native_word(colour_warp_address) != 0U;
                if (projected_points_address != 0U) {
                    pose.projected_points_address = static_cast<std::uint16_t>(
                        projected_points_address);
                }
                pose.explosion_progress = (object.flags & 0x01U) != 0U
                    ? object.count : 0U;
                if (game.flow_state()
                        == starfox::simulation::GameFlowState::intro
                    && display_width > snes_width) {
                    // The cinematic's smoke, fireball, and particle spawners
                    // assume the 256-pixel cartridge camera. Extending their
                    // visibility with the 3D scene reveals random off-camera
                    // effects in ultrawide modes, so retain that source mask
                    // for transient effects only.
                    pose.effect_clip_left = viewport_origin;
                    pose.effect_clip_right = viewport_origin
                        + static_cast<std::int32_t>(snes_width);
                }
                // RELFASTELASER is a long tapered solid. Near the intro
                // camera, clipping its broad tail through z=0 exposes a
                // screen-filling triangle. The captured cartridge sequence
                // retains only the beam axis at that crossing.
                pose.collapse_to_axis_line = game.flow_state()
                        == starfox::simulation::GameFlowState::intro
                    && object.shape == intro_laser_shape
                    && position.z < 1'024.0;
                starfox::render::apply_source_depth_tables(rom,
                    depth_table_address, depth_thresholds, depth_colours,
                    object.extended[21], pose);
                return pose;
            };
            // mshowview traverses the complete ordered list once for shadows,
            // then traverses it again for normal objects.
            const auto hardware_ray_tracing = game.ray_tracing()
                && game.renderer_mode()==starfox::simulation::RendererMode::gpu
                && std::getenv("STARFOX_DISABLE_DXR")==nullptr && dxr_shadows.available();
            const bool test_portable_shadows=test_frames && game.ray_tracing()
                && std::getenv("STARFOX_TEST_FORCE_PORTABLE_SHADOWS")!=nullptr;
            // Models also appear in Training, intros, title/control screens,
            // and roll calls. Ray-traced visibility is a renderer option, not
            // a gameplay-flow option. Ground remains source-controlled below.
            const auto enhanced_shadows_active = hardware_ray_tracing || test_portable_shadows;
            // Mutually exclusive: ray-traced receiver shadows replace the
            // cartridge silhouettes, never draw over a second shadow pass.
            if (shadows_enabled && !enhanced_shadows_active) {
                for (const auto& item : visible) {
                    const auto& object = game.objects().at(item.handle);
                    if ((object.strategy_flags[0] & 0x0cU) == 0U) continue;
                    const auto colour_table = effective_colour_table(object);
                    const auto base_shape_key =
                        (static_cast<std::uint32_t>(object.shape) << 16U)
                        | colour_table;
                    const auto base = shape_cache.find(base_shape_key);
                    if (base == shape_cache.end()) continue;
                    const auto shadow_pointer = base->second.header.shadow_pointer;
                    const auto shape_key =
                        (static_cast<std::uint32_t>(shadow_pointer) << 16U)
                        | colour_table;
                    if (invalid_shapes.contains(shape_key)) continue;
                    auto found = shape_cache.find(shape_key);
                    if (found == shape_cache.end()) {
                        try {
                            found = shape_cache.emplace(shape_key, decoder.decode_lod(
                                base->second.header, shadow_pointer,
                                colour_table)).first;
                        } catch (const std::exception&) {
                            invalid_shapes.insert(shape_key);
                            continue;
                        }
                    }
                    auto& target = controls_screen && item.handle == game.player()
                        ? controls_player_layer : superfx_frame;
                    draw_model(found->second, make_pose(item, true), target, false);
                }
            }
            for (const auto& item : visible) {
                const auto& object = game.objects().at(item.handle);
                if ((object.strategy_flags[0] & 0x04U) != 0U) continue;
                auto& target = controls_screen && item.handle == game.player()
                    ? controls_player_layer : superfx_frame;
                if ((object.strategy_flags[0] & 0x40U) != 0U) {
                    if(record_models && &target==&superfx_frame) {
                        auto text=text_renderer.prepare_projected(object.colour_table,object.extended[21],
                            std::bit_cast<std::int8_t>(object.texture_scroll_x),make_pose(item,false));
                        if(!text.glyphs.empty() && std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"GPU projected text recorded\n";
                        recorded_scene.append_text(raster_commands,{std::move(text),target.draw_scale()});
                    } else {
                        text_renderer.draw(object.colour_table, object.extended[21],
                            std::bit_cast<std::int8_t>(object.texture_scroll_x),make_pose(item, false), target);
                    }
                    continue;
                }
                const auto colour_table = effective_colour_table(object);
                const auto base_shape_key = (static_cast<std::uint32_t>(object.shape) << 16U)
                    | colour_table;
                if (object.shape == 0 || invalid_shapes.contains(base_shape_key)) continue;
                const auto base = shape_cache.find(base_shape_key);
                if (base == shape_cache.end()) continue;
                if ((object.strategy_flags[0] & 0x10U) != 0U) {
                    if(record_models && &target==&superfx_frame) {
                        auto particles=starfox::render::ParticleRenderer::prepare_owner(game.particles(),item.handle,
                            make_pose(item,false),interpolation_alpha);
                        recorded_scene.append_particles(raster_commands,{std::move(particles),target.draw_scale()});
                    } else {
                        particle_renderer.draw_owner(game.particles(), item.handle,
                            make_pose(item, false), interpolation_alpha, target);
                    }
                    continue;
                }
                const auto base_header = base->second.header;
                const auto selected_pointer = starfox::assets::ShapeDecoder::select_lod_pointer(
                    base_header, item.source_depth);
                const auto shape_key = (static_cast<std::uint32_t>(selected_pointer) << 16U)
                    | colour_table;
                auto found = shape_cache.find(shape_key);
                if (found == shape_cache.end()) {
                    try {
                        found = shape_cache.emplace(shape_key, decoder.decode_lod(
                            base_header, selected_pointer, colour_table)).first;
                    } catch (const std::exception&) {
                        invalid_shapes.insert(shape_key);
                        continue;
                    }
                }
                auto pose = make_pose(item, false);
                if (ex_crosshair_strategy_address != 0U
                    && object.strategy_address == ex_crosshair_strategy_address) {
                    pose.palette_override = 128U + 4U * 16U + 15U;
                    pose.colour_warp = false;
                }
                if ((object.strategy_flags[0] & 0x20U) != 0U) {
                    auto size_adjustment = static_cast<std::int16_t>(
                        std::bit_cast<std::int8_t>(object.texture_scroll_x));
                    for (std::uint8_t shift = 0; shift < base_header.shift; ++shift) {
                        size_adjustment = starfox::simulation::add16(
                            size_adjustment, size_adjustment);
                    }
                    auto diameter = starfox::simulation::add16(
                        base_header.size, size_adjustment);
                    diameter = starfox::simulation::add16(diameter, diameter);
                    if (diameter == 0) diameter = 1;
                    pose.simple_scaled_sprite = true;
                    pose.simple_sprite_colour = object.extended[21];
                    pose.simple_sprite_world_size = diameter;
                }
                draw_model(found->second, pose, target, false,
                    &target == &superfx_frame
                            && surface_effects
                        ? &superfx_surfaces : nullptr,
                    enhanced_shadows_active ? &shadow_scene : nullptr,
                    starfox::render::GpuModelIdentity{item.handle,
                        game.objects().generation(item.handle), object.shape,
                        object.strategy_address, object.type});
            }
            const auto render_model_shadows = [&](bool force_mono=false) {
            if (enhanced_shadows_active) {
                const auto ensure_cpu_casters=[&] {
                    if(cpu_casters_collected) return;
                    if(std::getenv("STARFOX_TRACE_GPU_RAYS")) std::cerr<<"ray-scene CPU caster fallback\n";
                    if(record_models) for(const auto& draw:recorded_scene.draws())
                        if(const auto* model=std::get_if<starfox::render::GpuModelDraw>(&draw);model && model->ray_geometry)
                            renderer.collect_shadow_casters(*model->shape,model->pose,shadow_scene);
                    cpu_casters_collected=true;
                };
                const auto light=world_to_camera(camera.x-1,camera.y-1,camera.z-1,camera,view_matrix);
                std::optional<starfox::render::shadows::ReceiverPlane> ground;
                if (shadows_enabled) {
                    const auto point=world_to_camera(camera.x,shadow_height,camera.z,camera,view_matrix);
                    const auto normal=world_to_camera(camera.x,camera.y+1,camera.z,camera,view_matrix);
                    ground=starfox::render::shadows::ReceiverPlane{
                        {point.x,point.y,point.z},{normal.x,normal.y,normal.z}};
                }
                const starfox::render::shadows::Camera shadow_camera{
                    superfx_frame.stored_width(),superfx_frame.stored_height(),256.0*render_scale,
                        static_cast<double>(game.map().read_native_word(vanish_x_address)+superfx_ui_offset_x)*render_scale,
                        static_cast<double>(game.map().read_native_word(vanish_y_address)
                            +(extend_scene_vertical?superfx_offset_y:0))*render_scale};
                const bool diagnostic_shadow_download=test_frames
                    && (std::getenv("STARFOX_TEST_SHADOW_REFERENCE")
                        || std::getenv("STARFOX_TEST_SHADOW_MASK")
                        || std::getenv("STARFOX_TEST_STEREO_SHADOW_DOWNLOAD"));
                const bool resident_casters=resident_raster && record_models && ray_scene_complete;
                mono_shadows_deferred=!force_mono && resident_casters && hardware_ray_tracing
                    && !diagnostic_shadow_download && game.stereo_output()!=0U;
                bool hardware=hardware_ray_tracing,portable=false;
                if(!mono_shadows_deferred) {
                if(!resident_casters || diagnostic_shadow_download) ensure_cpu_casters();
                if(hardware_ray_tracing && !diagnostic_shadow_download) {
                    resident_shadow=window.submit_shadows(shadow_scene,shadow_camera,
                        {light.x,light.y,light.z},ground,true,resident_casters);
                    if(!resident_shadow && resident_casters) {
                        ensure_cpu_casters();
                        resident_shadow=window.submit_shadows(shadow_scene,shadow_camera,{light.x,light.y,light.z},ground,true);
                    }
                }
                if(!resident_shadow) ensure_cpu_casters();
                hardware=hardware_ray_tracing && (resident_shadow
                    || dxr_shadows.render(shadow_scene,shadow_camera,
                        {light.x,light.y,light.z},ground,shadow_mask));
                if(hardware && test_frames && std::getenv("STARFOX_TEST_SHADOW_REFERENCE")) {
                    // Compare identical live geometry/camera/light, separating
                    // hardware tracing regressions from presentation changes.
                    shadow_scene.build();std::vector<std::uint8_t> reference;
                    starfox::render::shadows::render_mask(shadow_scene,shadow_camera,
                        {light.x,light.y,light.z},ground,reference,&shadow_workers);
                    if(presented_frames+1U==test_frames) {
                        std::size_t different=0;unsigned maximum=0;
                        for(std::size_t i=0;i<reference.size();++i) {
                            different+=reference[i]!=shadow_mask[i];
                            maximum=std::max(maximum,unsigned(std::abs(int(reference[i])-int(shadow_mask[i]))));
                        }
                        std::cerr<<"shadow-reference: differing="<<different<<"/"<<reference.size()
                            <<" max_delta="<<maximum<<" light="<<light.x<<','<<light.y<<','<<light.z<<'\n';
                        // DXR consumes float vertices. Separate that conversion
                        // from receiver/shadow-ray arithmetic when diagnosing
                        // grazing or tilted surfaces.
                        starfox::render::shadows::Scene float_scene;
                        const auto quantized=[](starfox::render::shadows::Vec3 p) {
                            return starfox::render::shadows::Vec3{float(p.x),float(p.y),float(p.z)};
                        };
                        for(const auto& triangle:shadow_scene.triangles())
                            float_scene.add({quantized(triangle.a),quantized(triangle.b),quantized(triangle.c)});
                        float_scene.build();std::vector<std::uint8_t> float_reference;
                        starfox::render::shadows::render_mask(float_scene,shadow_camera,
                            {light.x,light.y,light.z},ground,float_reference,&shadow_workers);
                        different=0;maximum=0;
                        for(std::size_t i=0;i<float_reference.size();++i) {
                            different+=float_reference[i]!=shadow_mask[i];
                            maximum=std::max(maximum,unsigned(std::abs(int(float_reference[i])-int(shadow_mask[i]))));
                        }
                        std::cerr<<"shadow-float-geometry-reference: differing="<<different
                            <<"/"<<float_reference.size()<<" max_delta="<<maximum<<'\n';
                    }
                    shadow_mask=std::move(reference);
                }
                if (!hardware) {
                    shadow_scene.build();
                    if(game.renderer_mode()==starfox::simulation::RendererMode::gpu
                        && std::getenv("STARFOX_DISABLE_PORTABLE_SHADOWS")==nullptr) {
                        resident_shadow=window.submit_shadows(shadow_scene,shadow_camera,
                            {light.x,light.y,light.z},ground);
                        portable=resident_shadow || portable_shadows.render(shadow_scene,shadow_camera,
                            {light.x,light.y,light.z},ground,shadow_mask);
                    }
                    if(!portable) starfox::render::shadows::render_mask(shadow_scene,shadow_camera,
                            {light.x,light.y,light.z},ground,shadow_mask,&shadow_workers);
                }
                }
                std::string status=hardware?(resident_shadow?(window.shadow_gpu_geometry()?"GPU-resident hardware DXR shadows (GPU caster geometry)":"GPU-resident hardware DXR shadows"):dxr_shadows.status()):resident_shadow?"GPU resident compute shadows":portable?portable_shadows.status():
                    game.renderer_mode()==starfox::simulation::RendererMode::software
                        ?"CPU shadows (software renderer)":"CPU shadows: "+portable_shadows.status();
                if(game.stereo_output()!=0U && !force_mono) {
                    bool stereo_hardware=true;
                    for(unsigned eye=0;eye<2;++eye) {
                        const double eye_x=eye?3.2:-3.2;
                        const starfox::render::shadows::Vec3 offset{-eye_x,0,0};
                        starfox::render::shadows::Scene eye_scene;
                        auto eye_camera=shadow_camera;
                        eye_camera.center_x+=eye_camera.focal_length*eye_x/512.;
                        auto eye_ground=ground;
                        if(eye_ground) eye_ground->point=eye_ground->point+offset;
                        if(hardware_ray_tracing && !diagnostic_shadow_download && resident_casters)
                            stereo_resident_shadow[eye]=window.submit_stereo_shadows(eye,
                                eye_scene,eye_camera,{light.x,light.y,light.z},eye_ground,true,
                                true);
                        if(!stereo_resident_shadow[eye]) {
                            ensure_cpu_casters();
                            for(const auto& triangle:shadow_scene.triangles())
                                eye_scene.add({triangle.a+offset,triangle.b+offset,triangle.c+offset});
                            if(hardware_ray_tracing && !diagnostic_shadow_download)
                                stereo_resident_shadow[eye]=window.submit_stereo_shadows(eye,eye_scene,eye_camera,{light.x,light.y,light.z},eye_ground,true);
                        }
                        const bool eye_hardware=hardware_ray_tracing && (stereo_resident_shadow[eye] || dxr_shadows.render(eye_scene,eye_camera,
                            {light.x,light.y,light.z},eye_ground,stereo_shadow_masks[eye]));
                        stereo_hardware&=eye_hardware;
                        if(!eye_hardware) {
                            eye_scene.build();
                            // Match the mono fallback: a failed DXR dispatch
                            // need not force two full CPU ray-tracing passes.
                            bool eye_portable=false;
                            if(game.renderer_mode()==starfox::simulation::RendererMode::gpu
                                && std::getenv("STARFOX_DISABLE_PORTABLE_SHADOWS")==nullptr) {
                                stereo_resident_shadow[eye]=!(test_frames
                                    && std::getenv("STARFOX_TEST_STEREO_SHADOW_DOWNLOAD"))
                                    && window.submit_stereo_shadows(eye,
                                        eye_scene,eye_camera,{light.x,light.y,light.z},eye_ground);
                                eye_portable=stereo_resident_shadow[eye]
                                    || portable_shadows.render(eye_scene,eye_camera,
                                        {light.x,light.y,light.z},eye_ground,stereo_shadow_masks[eye]);
                            }
                            if(!eye_portable) starfox::render::shadows::render_mask(eye_scene,eye_camera,
                                {light.x,light.y,light.z},eye_ground,stereo_shadow_masks[eye],&shadow_workers);
                        }
                    }
                    if(mono_shadows_deferred) status=stereo_hardware
                        ?(window.stereo_shadow_gpu_geometry(0) && window.stereo_shadow_gpu_geometry(1)
                            ?"GPU-resident hardware DXR shadows (stereo GPU caster geometry)":"GPU-resident hardware DXR shadows (stereo CPU caster geometry)")
                        :"Stereo shadow fallback (CPU/compute)";
                    if(test_frames && presented_frames+1U==test_frames && std::getenv("STARFOX_TRACE_GPU"))
                        std::cerr<<"stereo-shadow-resident: "<<stereo_resident_shadow[0]
                            <<','<<stereo_resident_shadow[1]<<'\n';
                }
                if (status!=shadow_backend_status) {
                    std::cerr << "shadow-backend: " << status << '\n';
                    shadow_backend_status=status;
                }
                if (test_frames && presented_frames+1U==test_frames
                    && std::getenv("STARFOX_TRACE_GPU") && !shadow_mask.empty()) {
                    // Test-only receiver classification: a changing frame/hash
                    // can otherwise prove only removal of the native shadow.
                    shadow_scene.build();
                    std::size_t ground_pixels=0, model_pixels=0;
                    for (unsigned y=0;y<shadow_camera.height;++y)
                        for (unsigned x=0;x<shadow_camera.width;++x) {
                            if (!shadow_mask[std::size_t(y)*shadow_camera.width+x]) continue;
                            const starfox::render::shadows::Vec3 ray{
                                (x+.5-shadow_camera.center_x)/shadow_camera.focal_length,
                                (y+.5-shadow_camera.center_y)/shadow_camera.focal_length,1};
                            double depth=65536;
                            bool on_ground=false;
                            if (ground) {
                                const auto denominator=dot(ray,ground->normal);
                                if (std::abs(denominator)>1e-10) {
                                    const auto distance=dot(ground->point,ground->normal)/denominator;
                                    if (distance>1 && distance<depth) {depth=distance;on_ground=true;}
                                }
                            }
                            if (shadow_scene.nearest({},ray,1,depth)) on_ground=false;
                            if (on_ground) ++ground_pixels; else ++model_pixels;
                        }
                    std::cerr<<"shadow-receivers: ground="<<ground_pixels
                        <<" model="<<model_pixels<<'\n';
                    if(const auto* path=std::getenv("STARFOX_TEST_SHADOW_MASK")) {
                        starfox::render::Framebuffer diagnostic(shadow_camera.width,shadow_camera.height);
                        std::array<starfox::render::Rgba8,256> greys{};
                        for(unsigned i=0;i<256;++i) greys[i]={uint8_t(i),uint8_t(i),uint8_t(i),255};
                        for(unsigned y=0;y<shadow_camera.height;++y) for(unsigned x=0;x<shadow_camera.width;++x)
                            diagnostic.set(x,y,shadow_mask[std::size_t(y)*shadow_camera.width+x]);
                        starfox::render::write_bmp(diagnostic,path,greys);
                    }
                }
            }
            };
            if (game.flow_state()
                    == starfox::simulation::GameFlowState::ex_pregame_menu
                || game.flow_state()
                    == starfox::simulation::GameFlowState::continue_choice) {
                const auto& native_model = game.map().native_model_draw();
                if (native_model.active && native_model.shape != 0U) {
                    const auto shape_key =
                        (static_cast<std::uint32_t>(native_model.shape) << 16U)
                        | native_model.colour_table;
                    if (!invalid_shapes.contains(shape_key)) {
                        auto found = shape_cache.find(shape_key);
                        if (found == shape_cache.end()) {
                            try {
                                found = shape_cache.emplace(shape_key,
                                    decoder.decode(native_model.shape, {},
                                        native_model.colour_table)).first;
                            } catch (const std::exception&) {
                                invalid_shapes.insert(shape_key);
                            }
                        }
                        if (found != shape_cache.end()) {
                            starfox::render::RenderPose pose;
                            pose.x = native_model.x;
                            pose.y = native_model.y;
                            // Model-viewer shoulder zoom updates M_BIGZ in
                            // CONTINUE.ASM after the Super FX draw snapshot is
                            // launched. Read that live source word so every
                            // visible presentation reflects the new distance.
                            pose.z = native_model_z_address != 0U
                                ? static_cast<std::int16_t>(
                                    game.map().read_native_word(
                                        native_model_z_address))
                                : native_model.z;
                            pose.pitch = static_cast<std::uint16_t>(
                                (native_model.rotation_x & 0x00ffU) << 8U);
                            pose.yaw = static_cast<std::uint16_t>(
                                (native_model.rotation_y & 0x00ffU) << 8U);
                            pose.roll = static_cast<std::uint16_t>(
                                (native_model.rotation_z & 0x00ffU) << 8U);
                            pose.rotation_matrix =
                                starfox::simulation::rotation_matrix_q15(
                                    trigonometry,
                                    static_cast<std::int16_t>(pose.pitch),
                                    static_cast<std::int16_t>(pose.yaw),
                                    static_cast<std::int16_t>(pose.roll));
                            pose.use_rotation_matrix = true;
                            // MSHOWOBJ3 (the source model viewer) enters
                            // MSHOWOBJECT directly; only the normal draw-list
                            // MSHOWOBJ2 path applies Huge Models.
                            pose.scale = 1.0;
                            pose.vanish_x = native_model.vanish_x
                                + superfx_ui_offset_x;
                            pose.vanish_y = native_model.vanish_y;
                            pose.animation_frame = native_model.animation_frame;
                            pose.colour_frame = native_model.colour_frame;
                            pose.wireframe_mode = wire_mode_address != 0U
                                ? game.map().read_native_byte(wire_mode_address)
                                : 0U;
                            pose.wobble_mode = wobble_mode_address != 0U
                                ? game.map().read_native_byte(wobble_mode_address)
                                : 0U;
                            pose.wave_mode = wave_mode_address != 0U
                                && game.map().read_native_byte(wave_mode_address)
                                    != 0U;
                            pose.cel_mode = cel_mode_address != 0U
                                && game.map().read_native_byte(cel_mode_address)
                                    != 0U;
                            pose.wave_offset = wave_offset_address != 0U
                                ? static_cast<std::int16_t>(
                                    game.map().read_native_word(
                                        wave_offset_address))
                                : 0;
                            pose.colour_warp = colour_warp_address != 0U
                                && game.map().read_native_word(
                                    colour_warp_address) != 0U;
                            if (projected_points_address != 0U) {
                                pose.projected_points_address =
                                    static_cast<std::uint16_t>(
                                        projected_points_address);
                            }
                            starfox::render::apply_source_depth_tables(rom,
                                depth_table_address, depth_thresholds,
                                depth_colours, 0U, pose);
                            draw_model(
                                found->second, pose, superfx_frame, false,
                                surface_effects
                                    ? &superfx_surfaces : nullptr,
                                enhanced_shadows_active ? &shadow_scene : nullptr);
                        }
                    }
                }
            }
            if (gameplay_hud && (current_cockpit_roll & 0x8000U) != 0U) {
                // In cockpit mode the player model can be absent from
                // `visible`. Interpolate the actual HUD state independently
                // and retain fractional angles through line rasterization.
                const auto hud_angle = starfox::timing::interpolate_cockpit_roll(
                    previous_cockpit_roll, current_cockpit_roll, interpolation_alpha);
                // INIT_STRATS enables MHUD only while the player is inside
                // the cockpit. It is a source Super FX line pass, so place it
                // above world models but below the complete SNES OBJ HUD.
                renderer.draw_cockpit_hud(
                    trigonometry,
                    hud_angle,
                    game.map().read_native_byte(hud_colour_address),
                    game.map().read_native_byte(hud_flags_address),
                    superfx_ui_offset_x,
                    superfx_hud,
                    crosshair_tint(game.crosshair_colour())
                        ? static_cast<std::uint8_t>(
                            128U + 4U * 16U + 15U)
                        : 0U);
            }
            const auto dialogue = game.dialogue_state();
            if (dialogue.active && !suppress_configurable_hud) {
                text_renderer.draw_face(
                    dialogue.portrait_frame, 48, 152, comms_hud,
                    7U * 16U, dialogue.alternate_portraits, display_width>snes_width);
                if (dialogue.text_visible) {
                    auto text_y = dialogue.three_lines ? 153 : 169;
                    const auto translated_lines=text_renderer.translated_game_text_lines(
                        dialogue.text_address,92).size();
                    if (translated_lines != 0)
                        text_y=std::min(text_y,183-10*(static_cast<int>(translated_lines)-1));
                    text_renderer.draw_game_text(dialogue.text_address,
                        83, text_y + 1, comms_hud, 7U * 16U, 9U, 175);
                    text_renderer.draw_game_text(dialogue.text_address,
                        82, text_y, comms_hud, 7U * 16U, std::nullopt, 174);
                }
                if(dialogue.meter_visible) {
                    // FRIENDS_MESSAGES_L only launches MSHOWTEAMMATE2 when
                    // FRIENDS_METER is active; ordinary calls have no meter.
                    for(int y=0;y<12;++y) for(int x=0;x<44;++x) {
                        if(x==0 || x==43 || y==0 || y==11)
                            comms_hud.set(82+x,177+y,126U);
                        else if(x>=2 && x<2+dialogue.meter_health && y>=2 && y<10)
                            comms_hud.set(82+x,177+y,114U);
                    }
                }
            }
            if (results.visible
                && game.experience()
                    != starfox::simulation::Experience::starfox_ex) {
                text_renderer.draw_game_text(
                    score_text, 16, 24, superfx_ui);
                text_renderer.draw_game_text(
                    total_score_text, 16, 40, superfx_ui);
                text_renderer.draw_game_text(
                    team_text, 48, 69, superfx_ui);
                sprite_renderer.draw_completion_bar(results.displayed_percentage, superfx_ui);
                const auto percentage_text = std::to_string(results.displayed_percentage) + "%";
                const auto total_text = std::to_string(results.total_percentage * 100U);
                // Align both values with the right edge of Slippy's frame.
                text_renderer.draw_ascii(percentage_text,
                    208 - text_renderer.measure_ascii(percentage_text), 24, superfx_ui);
                text_renderer.draw_ascii(total_text,
                    208 - text_renderer.measure_ascii(total_text), 40, superfx_ui);
                constexpr std::array<std::int32_t, 3> face_x{16, 96, 176};
                constexpr std::array<std::int32_t, 3> bar_x{11, 91, 171};
                constexpr std::array<std::int32_t, 3> name_x{15, 96, 173};
                constexpr std::array<std::int32_t, 3> down_x{11, 91, 170};
                constexpr std::array<std::uint8_t, 3> live_face_frame{
                    7U, 9U, 11U};
                for (std::size_t teammate = 0; teammate < 3U; ++teammate) {
                    const auto alive = results.teammate_health[teammate] != 0U;
                    const auto face_frame = alive
                        ? live_face_frame[teammate]
                        : static_cast<std::uint8_t>(
                            (game.map().read_native_byte(game_frame_address) & 1U)
                                != 0U ? 4U : 17U);
                    text_renderer.draw_face(
                        face_frame, face_x[teammate], 96, superfx_ui);
                    if (alive) {
                        text_renderer.draw_game_text(teammate_text[teammate],
                            name_x[teammate], 152, superfx_ui);
                        for (std::int32_t y = 138; y < 150; ++y) {
                            for (std::int32_t x = bar_x[teammate];
                                 x < bar_x[teammate] + 44; ++x) {
                                const auto border = y == 138 || y == 149
                                    || x == bar_x[teammate]
                                    || x == bar_x[teammate] + 43;
                                const auto filled = y >= 140 && y < 148
                                    && x >= bar_x[teammate] + 2
                                    && x - bar_x[teammate] - 2
                                        < std::min<std::uint8_t>(
                                            results.teammate_health[teammate], 40U);
                                superfx_ui.set(x, y, static_cast<std::uint8_t>(
                                    7U * 16U + (border ? 14U
                                        : (filled ? 2U : 0U))));
                            }
                        }
                    } else {
                        text_renderer.draw_game_text(teammate_text[teammate],
                            name_x[teammate], 137, superfx_ui);
                        text_renderer.draw_game_text(teammate_down_text,
                            down_x[teammate], 151, superfx_ui);
                    }
                }
            }
            if (results.visible && game.experience()
                    == starfox::simulation::Experience::starfox_ex) {
                // MCOPYFACE is host-composited, unlike EX's native tally text
                // and meters. Honor MAIN.ASM's NAMEGFXPOS table and exact
                // live/dead face choices instead of losing the three portraits.
                // MCOPYFACE's X is an 8-pixel tile column; Y is already pixels.
                constexpr std::array<std::uint8_t, 3> alive_frames{7U, 9U, 11U};
                for (std::size_t teammate = 0; teammate < alive_frames.size(); ++teammate) {
                    const auto frame = results.teammate_health[teammate] != 0U
                        ? alive_frames[teammate]
                        : static_cast<std::uint8_t>(
                            (game.map().read_native_byte(game_frame_address) & 1U) != 0U ? 4U : 17U);
                    text_renderer.draw_face(frame,
                        8 * rom.read8(teammate_face_positions + static_cast<std::uint32_t>(teammate * 2U)),
                        rom.read8(teammate_face_positions + static_cast<std::uint32_t>(teammate * 2U + 1U)),
                        superfx_ui);
                }
            }
            if (game.paused()) {
                text_renderer.draw_game_text(
                    pause_text, 90, 90, superfx_ui);
            }
            // A full-width layer keeps custom meter placements unclipped in
            // every aspect ratio. The default full-width coordinates are
            // pixel-identical to the former centred 224-pixel path at 4:3.
            if (!suppress_configurable_hud) {
                auto preview_meters = game.meter_state();
                if (hud_editor.active) {
                    // The editor must expose complete, independent shield,
                    // boost/bomb, and boss groups regardless of the exact
                    // cartridge tick selected for the frozen background.
                    preview_meters.enabled = true;
                    preview_meters.extended = false;
                    preview_meters.damage = 30U;
                    preview_meters.boost = 26U;
                    preview_meters.shield_up = false;
                    preview_meters.boost_enabled = true;
                    preview_meters.player_two_activated = false;
                    preview_meters.second_player_view = false;
                    preview_meters.player_one_dead = false;
                    preview_meters.boss_max_health = 0xffU;
                    preview_meters.boss_health = 180U;
                }
                sprite_renderer.draw_meters(
                    preview_meters, superfx_hud, true,
                    stage_hud ? &active_hud_layout : nullptr);
                if (hud_editor.active) {
                    constexpr std::int32_t preview_boss_meter_width = 131;
                    const auto boss_offset = active_hud_layout[
                        starfox::render::HudElement::boss_health];
                    const auto boss_x = static_cast<std::int32_t>(
                        superfx_hud.width()) - 18
                        - preview_boss_meter_width + boss_offset.x;
                    constexpr std::string_view enemy_label{"ENEMY"};
                    text_renderer.draw_ascii(enemy_label,
                        boss_x - text_renderer.measure_ascii(enemy_label) - 1,
                        2 + boss_offset.y, superfx_hud, 14U);
                }
            }

            if(record_raster) {
                superfx_frame.record_to(nullptr);
                auto* metadata=surface_effects?&superfx_surfaces:nullptr;
                if(record_models) {
                    for(const auto& model:controls_model_draws) recorded_scene.append_model(raster_commands,model);
                    if(!controls_model_draws.empty() && std::getenv("STARFOX_TRACE_GPU")) std::cerr<<"GPU Controls player layer recorded\n";
                    recorded_scene.finish(raster_commands);
                    resident_raster=!std::getenv("STARFOX_CAPTURE_INDEXED_PATH")
                        && window.submit_scene(recorded_scene,superfx_frame.stored_width(),superfx_frame.stored_height(),game.stereo_output());
                    if(!resident_raster) recorded_scene.replay(superfx_frame,metadata);
                } else {
                resident_raster=window.native_gpu_enabled()
                    && !std::getenv("STARFOX_CAPTURE_INDEXED_PATH")
                    && window.submit_native(raster_commands,surface_effects);
                if(!resident_raster && !gpu_raster.render(raster_commands,superfx_frame,metadata)) {
                    starfox::render::replay_raster_commands(raster_commands,superfx_frame,metadata);
                    gpu_raster_failed=true;
                }
                }
                if(!gpu_raster_reported || gpu_raster_failed) {
                    std::cerr<<"native-raster: "<<(resident_raster?"GPU resident":gpu_raster.status())<<'\n';gpu_raster_reported=true;
                }
            }
            // Ray inputs are the completed deferred batch, including the model
            // viewer/Continue ship. Never consume the preceding frame's buffers.
            render_model_shadows();
            const auto profile_world_done = std::chrono::steady_clock::now();
            // Colour zero is transparent in every host Super FX layer.
            const auto composite_superfx = [&framebuffer, viewport_origin, boss_roll,
                                                &ppu,&resident_raster,&resident_layer,&superfx_frame,
                                                &deferred_background,&background_cpu_coverage,&temporal_background](
                                               const auto& source,
                                               std::int32_t offset_x,
                                               std::int32_t offset_y,
                                               bool clip_controls) {
                // CONT.SCR's black flight panel is exactly 112x88 pixels;
                // the surrounding pixels belong to its bevelled frame.
                constexpr auto controls_top = 24;
                constexpr auto controls_bottom = 112;
                const auto controls_left = 24 + viewport_origin;
                const auto controls_right = 136 + viewport_origin;
                starfox::render::LayerCompositeSettings settings;
                settings.offset_x = offset_x;
                settings.offset_y = offset_y;
                // Every separated host SuperFX layer represents BG1 pixels.
                // Apply $2106 before compositing so EX's mosaic shortcut
                // affects host-rendered models, particles, meters and text in
                // exactly the same way as the cartridge bitmap.
                settings.mosaic = ppu.mosaic;
                settings.mosaic_layer_mask = 0x01U;
                settings.mosaic_origin_x = viewport_origin;
                if (clip_controls) {
                    settings.clip_left = controls_left;
                    settings.clip_right = controls_right;
                    settings.clip_top = controls_top;
                    settings.clip_bottom = controls_bottom;
                } else if (boss_roll) {
                    // ENDSEQ slides a 224x192 BG1 bitmap inside the original
                    // dossier canvas. Off-screen missiles must not escape
                    // into widescreen margins or cross its text panels.
                    settings.clip_left = std::max(viewport_origin,
                        viewport_origin + 16 - ppu.bg1_scroll_x);
                    settings.clip_right = std::min(viewport_origin + 256,
                        viewport_origin + 240 - ppu.bg1_scroll_x);
                    settings.clip_top = std::max(0, 16 - ppu.bg1_scroll_y);
                    settings.clip_bottom = std::min(224, 208 - ppu.bg1_scroll_y);
                }
                if(resident_raster && &source==&superfx_frame) {
                    if(std::getenv("STARFOX_TEST_DLSS_EVALUATE")) temporal_background=framebuffer;
                    if(deferred_background) background_cpu_coverage.assign(framebuffer.write_coverage().begin(),framebuffer.write_coverage().end());
                    resident_layer=settings;framebuffer.begin_write_coverage();return;
                }
                starfox::render::composite_transparent_layer(
                    source, framebuffer, settings);
            };

            if (controls_screen) {
                // CONT draws demo lasers and bombs before its player pass.
                // Keeping those effects in the ordinary foreground layer
                // painted them across the Arwing; composite them first, then
                // the isolated player, and finally the controller artwork.
                composite_superfx(
                    superfx_frame, 0, scene_offset_y, true);
                composite_superfx(
                    controls_player_layer, 0, superfx_offset_y, true);
                if (ppu.background_mode == 1U) {
                    background_renderer.draw_bg2(ppu, background_x,
                        background_y, framebuffer,
                        starfox::render::TilePriorityPass::high,
                        viewport_origin, false);
                }
            } else {
                composite_superfx(
                    superfx_frame, boss_roll ? -ppu.bg1_scroll_x : 0,
                    scene_offset_y - (boss_roll ? ppu.bg1_scroll_y : 0), false);
                if ((game.flow_state()
                        == starfox::simulation::GameFlowState::continue_choice || boss_roll)
                    && ppu.background_mode == 1U) {
                    // MSHOWOBJ3 supplies BG1. In the source Mode 1 priority
                    // order OBJ priority 2 and BG2-high (the Continue window
                    // frame) are above that bitmap. Restore those two passes
                    // after the host model so it remains behind the window.
                    sprite_renderer.draw_objects(ppu, framebuffer, 2U,
                        viewport_origin, extend_cartridge_scene);
                    background_renderer.draw_bg2(ppu, background_x,
                        background_y, framebuffer,
                        starfox::render::TilePriorityPass::high,
                        viewport_origin, extend_cartridge_scene);
                }
            }
            if (present_native_ex_bitmap) {
                // EX draws its scored/FPS/multiplayer diagnostics and full
                // interactive pause menu AND end-level tally into Super FX BG1.
                // The results task is not gameplay_hud: excluding it hid the
                // entire native tally even though its source timers ran.
                // Render the native Super FX BG1
                // bitmap. Render it into a transparent staging layer first:
                // source guard pixels can use non-zero palette entries whose
                // RGB value is black, and drawing those directly created 4:3
                // bars over the expanded EX world. The PC communication HUD
                // is authoritative while a message is active, avoiding a
                // second copy of the same EX portrait/text from this bitmap.
                // The outer 16-pixel columns are guards, not bitmap artwork.
                // Their palette can be tan during scramble; filtering only
                // RGB-black entries therefore leaves colored border strips.
                background_renderer.draw_bg1(ppu, native_ex_overlay,
                    starfox::render::TilePriorityPass::all,
                    0, false, 16U);
                if (gameplay_hud && dialogue.active && !game.paused()) {
                    native_ex_overlay.clear(0U);
                } else {
                    for (std::uint32_t y = 0U;
                         y < native_ex_overlay.height(); ++y) {
                        for (std::uint32_t x = 0U;
                             x < native_ex_overlay.width(); ++x) {
                            const auto index = native_ex_overlay.get(x, y);
                            if (index != 0U
                                && (ppu.cgram[index] & 0x7fffU) == 0U) {
                                native_ex_overlay.set(x, y, 0U);
                            }
                        }
                    }
                }
                composite_superfx(
                    native_ex_overlay, viewport_origin, 0, false);
            }
            composite_superfx(
                superfx_hud, 0, superfx_offset_y, false);
            if (stage_hud) {
                // The Super FX world is below the complete gameplay OBJ HUD.
                // The priority bits order HUD sprites against one another;
                // they do not place labels behind projected model faces.
                const auto live_meters = game.meter_state();
                for (std::uint8_t priority = 0U; priority < 4U; ++priority) {
                    sprite_renderer.draw_objects(ppu, framebuffer, priority,
                        viewport_origin, extend_cartridge_scene, anchor_edge_hud,
                        &active_hud_layout,
                        suppress_configurable_hud && gameplay_hud, &live_meters);
                }
            }
            const auto comms_offset = active_hud_layout[
                starfox::render::HudElement::comms];
            composite_superfx(comms_hud,
                superfx_ui_offset_x + comms_offset.x,
                superfx_offset_y + comms_offset.y, false);
            composite_superfx(
                superfx_ui, superfx_ui_offset_x, superfx_offset_y, false);

            if (game.flow_state() == starfox::simulation::GameFlowState::title
                && ppu.background_mode == 1U) {
                if(resident_raster && record_models && !std::getenv("STARFOX_DISABLE_GPU_LATE_CARTRIDGE")) {
                    late_cartridge=std::make_unique<DeferredBackground>();
                    late_cartridge->ppu=std::make_shared<const starfox::simulation::SnesPpuState>(ppu);
                    late_cartridge->tag=starfox::render::PixelLayer::two_d;
                    late_cartridge->scene.reset(framebuffer.stored_width(),framebuffer.stored_height());
                    late_cartridge->pending.reset(framebuffer.stored_width(),framebuffer.stored_height());
                    background_renderer.recording=late_cartridge.get();background_renderer.target=&framebuffer;
                    framebuffer.record_to(&late_cartridge->pending);
                }
                // Restore Mode 1 foreground priorities above the title model.
                // Retail PUSH START and its opaque outline are BG2 artwork;
                // the logo occupies high-priority BG3.
                // EX's introductory logo uses its whole BG1 bitmap for the
                // animation; the regular retail/EX title uses BG1 only for
                // source-authored text that must survive host model drawing.
                background_renderer.draw_title_foreground(ppu,
                    background_x, background_y, framebuffer, viewport_origin,
                    !ex_title_logo_screen, extend_ex_title_art);
            }

            // PLANET's briefing is copied through the full-width Mode 3
            // screen buffer rather than the inset Super FX character layer.
            const auto briefing = game.briefing_state();
            if (briefing.active) {
                // DOG.SCR selects BG2 palette bank 6 for the 4-bpp text
                // bitmap. M_TEXTCOLOUR is a nibble within that bank, not
                // an absolute Mode 3 CGRAM index.
                constexpr auto briefing_palette = starfox::render::briefing_text_palette_base;
                auto& briefing_target = planet_presentation.briefing_layers
                    ? planet_text_overlay : framebuffer;
                if (briefing.message_address != 0U) {
                    text_renderer.draw_game_text(briefing.message_address,
                        30 + viewport_origin, 173, briefing_target, briefing_palette, 5U,
                        218 + viewport_origin,
                        briefing.visible_message_characters);
                    text_renderer.draw_game_text(briefing.message_address,
                        28 + viewport_origin, 171, briefing_target, briefing_palette, 13U,
                        216 + viewport_origin,
                        briefing.visible_message_characters);
                }
                if (briefing.planet_name_address != 0U) {
                    text_renderer.draw_game_text(briefing.planet_name_address,
                        30 + viewport_origin, 41, briefing_target, briefing_palette, 1U,
                        224 + viewport_origin,
                        briefing.visible_planet_characters);
                    text_renderer.draw_game_text(briefing.planet_name_address,
                        28 + viewport_origin, 39, briefing_target, briefing_palette, 4U,
                        224 + viewport_origin,
                        briefing.visible_planet_characters);
                }
            }
            if ((ppu.main_screen & 0x10U) != 0U
                && !planet_presentation.briefing_layers
                && !gameplay_hud) {
                sprite_renderer.draw_objects(
                    ppu, framebuffer, 3U, viewport_origin,
                    extend_cartridge_scene, anchor_edge_hud);
            }
            if (ppu.background_mode == 1U && ppu.bg3_high_priority) {
                background_renderer.draw_bg3(
                    ppu, framebuffer, starfox::render::TilePriorityPass::high,
                    viewport_origin, extend_cartridge_scene);
            }
            if(late_cartridge) {
                framebuffer.record_to(nullptr);late_cartridge->scene.finish(late_cartridge->pending);
                background_renderer.recording=nullptr;background_renderer.target=nullptr;
            }
            const auto solid_frontend_margins = controls_screen
                || game.flow_state()
                    == starfox::simulation::GameFlowState::game_over
                || game.flow_state()
                    == starfox::simulation::GameFlowState::continue_choice;
            if (solid_frontend_margins && viewport_origin > 0 && !deferred_background) {
                // These front ends draw a single-colour field around their
                // centred artwork. Pick the dominant colour from each native
                // edge rather than extending every scanline independently:
                // a star or animated edge pixel must not become a full-width
                // horizontal line in the added margins.
                const auto right = viewport_origin
                    + static_cast<std::int32_t>(snes_width);
                const auto dominant_edge_colour = [&framebuffer](
                                                      std::int32_t x) {
                    std::array<std::uint32_t, 256U> counts{};
                    for (std::int32_t y = 0;
                         y < static_cast<std::int32_t>(framebuffer.height());
                         ++y) {
                        ++counts[framebuffer.get(x, y)];
                    }
                    return static_cast<std::uint8_t>(std::distance(
                        counts.begin(), std::max_element(
                            counts.begin(), counts.end())));
                };
                const auto left_backdrop = dominant_edge_colour(viewport_origin);
                const auto right_backdrop = dominant_edge_colour(right - 1);
                for (std::int32_t y = 0;
                     y < static_cast<std::int32_t>(framebuffer.height()); ++y) {
                    for (std::int32_t x = 0; x < viewport_origin; ++x) {
                        framebuffer.set(x, y, left_backdrop);
                    }
                    for (std::int32_t x = right;
                         x < static_cast<std::int32_t>(framebuffer.width()); ++x) {
                        framebuffer.set(x, y, right_backdrop);
                    }
                }
            }
            std::int32_t setup_overlay_left = 12, setup_overlay_right = 243;
            std::optional<starfox::render::DustRenderer::DustFrame> late_dust;
            if (game.flow_state() == starfox::simulation::GameFlowState::game_over
                && viewport_origin > 0 && game.map().dots_mode() < 0) {
                // Extend only world-space stars after the solid margin fill.
                // Keep the entire native canvas (Andross, text, priority)
                // untouched; do not tile or widen its background artwork.
                if(resident_raster && !std::getenv("STARFOX_DISABLE_GPU_LATE_DUST")) {
                    late_dust=dust_renderer.prepare_dust(game.dust(),game.dust_point_count(),camera,view_matrix);
                    late_dust->exclude_left=viewport_origin;
                    late_dust->exclude_right=viewport_origin+static_cast<std::int32_t>(snes_width);
                } else {
                    // CPU late dust must be drawn after the deferred margin
                    // fill too. Resolve that layer now instead of letting a
                    // later GPU reduction paint over already-drawn stars.
                    if(deferred_background) {
                        auto coverage=std::vector<std::uint8_t>(framebuffer.write_coverage().begin(),framebuffer.write_coverage().end());
                        if(!background_cpu_coverage.empty()) for(std::size_t i=0;i<coverage.size();++i) coverage[i]|=background_cpu_coverage[i];
                        restore_background(*deferred_background,framebuffer,coverage);
                        if(deferred_background->margin_origin && !deferred_background->repair_margins) fill_frontend_margins(framebuffer,deferred_background->margin_origin);
                        deferred_background.reset();background_cpu_coverage.clear();
                    }
                    dust_renderer.draw(game.dust(), game.dust_point_count(),camera,view_matrix,framebuffer,0,0,
                        viewport_origin,viewport_origin+static_cast<std::int32_t>(snes_width));
                }
            }
            if (!menu_peek && (game.flow_state()
                == starfox::simulation::GameFlowState::pregame_menu
                || game.menu_preview())) {
                setup_overlay.resize(display_width, snes_height);
                if (!game.menu_preview()) framebuffer.clear(0U);
                auto& setup_target = setup_overlay;
                auto& framebuffer = setup_target;
                framebuffer.clear(0U);
                constexpr auto border_colour = static_cast<std::uint8_t>(
                    7U * 16U + 4U);
                // Translated labels need breathing room at the same readable
                // font size, not narrower letters to fit the English column.
                const auto menu_extra = game.language() != 0U
                    && !hud_editor.active && !remap_menu.active
                    && game.pregame_page() != starfox::simulation::PregamePage::options
                    ? std::min<std::int32_t>(64, (display_width - snes_width) / 2) : 0;
                const std::int32_t menu_left = 12 - menu_extra;
                const std::int32_t menu_right = 243 + menu_extra;
                const std::int32_t menu_label_x = 32 - menu_extra;
                const std::int32_t menu_value_right = 236 + menu_extra;
                const std::int32_t menu_cursor_x = 20 - menu_extra;
                setup_overlay_left = menu_left;
                setup_overlay_right = menu_right;
                for (std::int32_t x = menu_left + viewport_origin;
                     x <= menu_right + viewport_origin; ++x) {
                    framebuffer.set(x, 20, border_colour);
                    framebuffer.set(x, 222, border_colour);
                }
                for (std::int32_t y = 20; y <= 222; ++y) {
                    framebuffer.set(menu_left + viewport_origin, y, border_colour);
                    framebuffer.set(menu_right + viewport_origin, y, border_colour);
                }
                const auto draw_centred = [&text_renderer, &framebuffer,
                                            viewport_origin](
                                               std::string_view text,
                                               std::int32_t y,
                                               std::uint8_t colour) {
                    text_renderer.draw_ascii(text,
                        128 - text_renderer.measure_ascii(text) / 2
                            + viewport_origin,
                        y, framebuffer, colour);
                };
                if (hud_editor.active) {
                    constexpr auto palette_base = static_cast<std::uint8_t>(
                        7U * 16U);
                    const auto solid = [&framebuffer](
                                           std::int32_t x, std::int32_t y,
                                           std::int32_t width, std::int32_t height,
                                           std::uint8_t colour) {
                        for (std::int32_t row = 0; row < height; ++row) {
                            for (std::int32_t column = 0; column < width; ++column) {
                                framebuffer.set(x + column, y + row, colour);
                            }
                        }
                    };
                    const auto box = [&solid](HudRect rect, std::uint8_t colour) {
                        solid(rect.x, rect.y, rect.width, 1, colour);
                        solid(rect.x, rect.y + rect.height - 1,
                            rect.width, 1, colour);
                        solid(rect.x, rect.y, 1, rect.height, colour);
                        solid(rect.x + rect.width - 1, rect.y,
                            1, rect.height, colour);
                    };
                    framebuffer.clear(0U);

                    const auto& editor_layout = hud_layouts[
                        hud_profile_index(
                            game.display_mode(), game.experience())];
                    std::optional<starfox::render::HudElement> hovered;
                    std::int32_t hovered_area = std::numeric_limits<std::int32_t>::max();
                    for (std::uint8_t value = 0U;
                         value < static_cast<std::uint8_t>(
                             starfox::render::HudElement::count); ++value) {
                        const auto element = static_cast<starfox::render::HudElement>(
                            value);
                        const auto rect = placed_hud_rect(
                            element, display_width, editor_layout,
                            game.experience());
                        const auto area = rect.width * rect.height;
                        if (rect.contains(hud_editor.pointer_x,
                                hud_editor.pointer_y) && area < hovered_area) {
                            hovered = element;
                            hovered_area = area;
                        }
                    }
                    const auto selected = hud_editor.dragging
                        ? hud_editor.dragging : hovered;
                    const auto corner_brackets = [&solid](
                                                     HudRect rect,
                                                     std::uint8_t colour) {
                        constexpr std::int32_t length = 5;
                        --rect.x;
                        --rect.y;
                        rect.width += 2;
                        rect.height += 2;
                        solid(rect.x, rect.y, length, 1, colour);
                        solid(rect.x, rect.y, 1, length, colour);
                        solid(rect.x + rect.width - length, rect.y,
                            length, 1, colour);
                        solid(rect.x + rect.width - 1, rect.y,
                            1, length, colour);
                        solid(rect.x, rect.y + rect.height - 1,
                            length, 1, colour);
                        solid(rect.x, rect.y + rect.height - length,
                            1, length, colour);
                        solid(rect.x + rect.width - length,
                            rect.y + rect.height - 1, length, 1, colour);
                        solid(rect.x + rect.width - 1,
                            rect.y + rect.height - length, 1, length, colour);
                    };
                    if (selected) {
                        corner_brackets(placed_hud_rect(*selected,
                            display_width, editor_layout, game.experience()),
                            static_cast<std::uint8_t>(palette_base + 14U));
                    }

                    // The preview itself is composited from a captured native
                    // gameplay frame below. This indexed layer is deliberately
                    // limited to unobtrusive editor chrome and drag handles.
                    solid(0, 0, static_cast<std::int32_t>(display_width),
                        11, static_cast<std::uint8_t>(palette_base + 1U));
                    const std::array<std::string_view, 3> title_parts{
                        "HUD LAYOUT", game.experience()
                            == starfox::simulation::Experience::starfox_ex ? "STARFOX EX" : "ORIGINAL",
                        display_profile_name(game.display_mode())};
                    auto title_width = 16;
                    for (const auto part : title_parts)
                        title_width += text_renderer.measure_ascii(part);
                    auto title_x = static_cast<std::int32_t>(display_width / 2U) - title_width / 2;
                    for (const auto part : title_parts) {
                        text_renderer.draw_ascii(part, title_x, 2, framebuffer, 14U);
                        title_x += text_renderer.measure_ascii(part) + 8;
                    }
                    solid(0, 210, static_cast<std::int32_t>(display_width),
                        14, static_cast<std::uint8_t>(palette_base + 1U));
                    const auto reset = hud_reset_button_rect(display_width);
                    const auto done = hud_done_button_rect(display_width);
                    if (reset.contains(hud_editor.pointer_x,
                            hud_editor.pointer_y)) {
                        box(reset, static_cast<std::uint8_t>(palette_base + 14U));
                    }
                    if (done.contains(hud_editor.pointer_x,
                            hud_editor.pointer_y)) {
                        box(done, static_cast<std::uint8_t>(palette_base + 14U));
                    }
                    text_renderer.draw_ascii("Y RESET", reset.x + (reset.width - text_renderer.measure_ascii("Y RESET")) / 2,
                        reset.y + 1, framebuffer, 15U);
                    text_renderer.draw_ascii("B DONE", done.x + (done.width - text_renderer.measure_ascii("B DONE")) / 2,
                        done.y + 1, framebuffer, 15U);
                } else if (remap_menu.active) {
                    draw_centred("CONTROLLER REMAP", 34, 14U);
                    draw_centred("D-PAD  CHOOSE", 51, 10U);
                    const auto device = remap_menu.device
                            == starfox::app::BindingDevice::keyboard
                        ? std::string{"KEYBOARD"}
                        : starfox::app::gamepad_device_label(gamepad);
                    draw_centred(device, 74, 13U);
                    const auto action_number = std::to_string(remap_menu.action + 1U) + "/"
                        + std::to_string(
                            starfox::app::InputBindings::remap_action_count(remap_menu.device));
                    // Translate each semantic field before laying out the row;
                    // a concatenated dynamic string cannot match catalog keys.
                    const std::array<std::string_view, 3> action_parts{
                        "ACTION", starfox::app::InputBindings::action_name(remap_menu.action),
                        action_number};
                    auto action_width = 16;
                    for (const auto part : action_parts)
                        action_width += text_renderer.measure_ascii(part);
                    auto action_x = 128 + viewport_origin - action_width / 2;
                    for (const auto part : action_parts) {
                        text_renderer.draw_ascii(part, action_x, 98, framebuffer, 14U);
                        action_x += text_renderer.measure_ascii(part) + 8;
                    }
                    auto binding = remap_menu.waiting_for_input
                        ? std::string{"PRESS A KEY OR CONTROL"}
                        : bindings.binding_name(
                            remap_menu.device, remap_menu.action);
                    if (binding.size() > 25U) binding.resize(25U);
                    draw_centred(binding, 116,
                        remap_menu.waiting_for_input ? 14U : 7U);
                    draw_centred("LEFT/RIGHT  DEVICE", 143, 13U);
                    draw_centred("A  BIND   Y  DEFAULTS", 158, 13U);
                    draw_centred("B/START/ESC  DONE", 177, 13U);
                } else {
                    if (game.pregame_page() == starfox::simulation::PregamePage::main) {
                        draw_centred("STAR FOX ENHANCED", 5, 14U);
                    }
                    const auto draw_cursor = [&framebuffer, viewport_origin,
                                                  menu_cursor_x](
                                                 std::int32_t y) {
                        for (std::int32_t column = 0; column < 5; ++column) {
                            const auto half_height = 4 - column;
                            for (std::int32_t row = -half_height;
                                 row <= half_height; ++row) {
                                framebuffer.set(menu_cursor_x + viewport_origin + column,
                                    y + row, static_cast<std::uint8_t>(
                                        7U * 16U + 14U));
                            }
                        }
                    };
                    const auto draw_row = [&text_renderer, &framebuffer,
                                              viewport_origin, menu_label_x,
                                              menu_value_right](
                                              std::string_view label,
                                              std::string_view value,
                                              std::int32_t y, bool selected) {
                        const auto colour = static_cast<std::uint8_t>(
                            selected ? 14U : 7U);
                        text_renderer.draw_ascii(label,
                            menu_label_x + viewport_origin,
                            y, framebuffer, colour);
                        if (!value.empty()) {
                            text_renderer.draw_ascii(value,
                                menu_value_right
                                    - text_renderer.measure_ascii(value)
                                    + viewport_origin,
                                y, framebuffer, colour);
                        }
                    };

                    if (game.pregame_page() == starfox::simulation::PregamePage::cheats) {
                        draw_centred("CHEATS", 27, 10U);
                        constexpr std::array<std::string_view, 3> lasers{"SINGLE", "DUAL", "BEAM"};
                        draw_row("GOD MODE", game.god_mode() ? "ON" : "OFF", 50, game.pregame_selection() == 0U);
                        draw_row("LEVEL SELECT", game.selected_level_name(), 76, game.pregame_selection() == 1U);
                        draw_row("DEFAULT LASER", lasers[game.default_laser()], 102, game.pregame_selection() == 2U);
                        draw_row("INFINITE BOMBS", game.infinite_bombs() ? "ON" : "OFF", 128, game.pregame_selection() == 3U);
                        draw_row("INFINITE BOOST", game.infinite_boost() ? "ON" : "OFF", 154, game.pregame_selection() == 4U);
                        draw_row("INFINITE LIVES", game.infinite_lives() ? "ON" : "OFF", 180, game.pregame_selection() == 5U);
                        draw_row("BACK", "A", 206, game.pregame_selection() == 6U);
                        constexpr std::array<std::int32_t, 7> cheat_cursor_y{53,79,105,131,157,183,209};
                        draw_cursor(cheat_cursor_y[game.pregame_selection()]);
                    } else if (game.pregame_page()
                        == starfox::simulation::PregamePage::options) {
                        draw_centred("OPTIONS", 5, 10U);
                        constexpr std::array<std::string_view,3> stereo_names{"OFF", "HALF SBS", "FULL SBS"};
                        draw_row("3D OUTPUT", stereo_names[game.stereo_output()], 25,
                            game.pregame_selection() == 9U);
                        const auto fps_value = game.show_fps()
                            ? std::string_view{"ON"} : std::string_view{"OFF"};
                        const auto crosshair = crosshair_colour_name(
                            game.crosshair_colour());
                        draw_row("CHEATS", "A  OPEN", 40,
                            game.pregame_selection() == 0U);
                        draw_row("ON-SCREEN FPS", fps_value, 55,
                            game.pregame_selection() == 1U);
                        draw_row("CROSSHAIR COLOR", crosshair, 70,
                            game.pregame_selection() == 2U);
                        draw_row("CUSTOMIZE SCREEN", "A  OPEN", 85,
                            game.pregame_selection() == 3U);
                        const auto music_volume =
                            std::to_string(game.music_volume()) + "%";
                        const auto sfx_volume =
                            std::to_string(game.sfx_volume()) + "%";
                        draw_row("ON-SCREEN BUTTONS",
                            game.on_screen_controls()
                                ? std::string_view{"ON"}
                                : std::string_view{"OFF"},
                            100, game.pregame_selection() == 4U);
                        draw_row("SWAP A/B + Y/X",
                            game.swap_face_buttons()
                                ? std::string_view{"ON"}
                                : std::string_view{"OFF"},
                            115, game.pregame_selection() == 5U);
                        draw_row("MUSIC VOLUME", music_volume, 130,
                            game.pregame_selection() == 6U);
                        draw_row("SFX VOLUME", sfx_volume, 154,
                            game.pregame_selection() == 7U);
                        draw_row("CONTROLLER", "A  REMAP", 170,
                            game.pregame_selection() == 8U);
                        constexpr std::array<std::string_view, 6> language_names{
                            "ENGLISH", "JAPANESE", "GERMAN", "FRENCH", "SPANISH", "ENGLISH (EUROPE)"};
                        draw_row("LANGUAGE", language_names[game.language()], 185,
                            game.pregame_selection() == 12U);
                        draw_row("BACK", "", 200,
                            game.pregame_selection() == 11U);
                        const auto draw_volume_bar = [&framebuffer,
                                                         viewport_origin](
                                                         std::int32_t y,
                                                         std::uint8_t volume,
                                                         bool selected) {
                            constexpr std::int32_t left = 147;
                            constexpr std::int32_t width = 89;
                            constexpr std::int32_t height = 6;
                            const auto border = static_cast<std::uint8_t>(
                                7U * 16U + (selected ? 14U : 7U));
                            const auto fill = static_cast<std::uint8_t>(
                                7U * 16U + 10U);
                            for (std::int32_t x = 0; x < width; ++x) {
                                framebuffer.set(left + viewport_origin + x,
                                    y, border);
                                framebuffer.set(left + viewport_origin + x,
                                    y + height - 1, border);
                            }
                            for (std::int32_t row = 1; row < height - 1; ++row) {
                                framebuffer.set(left + viewport_origin,
                                    y + row, border);
                                framebuffer.set(left + viewport_origin
                                    + width - 1, y + row, border);
                            }
                            const auto filled = static_cast<std::int32_t>(
                                (width - 2) * volume / 100U);
                            for (std::int32_t row = 1; row < height - 1; ++row) {
                                for (std::int32_t x = 1; x <= filled; ++x) {
                                    framebuffer.set(left + viewport_origin + x,
                                        y + row, fill);
                                }
                            }
                        };
                        draw_volume_bar(139, game.music_volume(),
                            game.pregame_selection() == 6U);
                        draw_volume_bar(163, game.sfx_volume(),
                            game.pregame_selection() == 7U);
                        constexpr std::array<std::int32_t, 13> cursor_y{
                            43, 58, 73, 88, 103, 118, 133, 157, 173, 28, 197, 203, 188};
                        draw_cursor(cursor_y[game.pregame_selection()]);
                    } else {
                        const auto timing = game.timing_mode()
                            == starfox::simulation::TimingMode::unlocked_20_fps
                            ? std::string_view{"UNLOCKED 20 HZ"}
                            : std::string_view{"ORIGINAL"};
                        const auto presentation =
                            std::to_string(game.presentation_fps()) + " FPS";
                        const auto display = [mode = game.display_mode()]()
                            -> std::string_view {
                            switch (mode) {
                            case starfox::simulation::DisplayMode::widescreen_16_9:
                                return "16 BY 9 WIDE";
                            case starfox::simulation::DisplayMode::widescreen_16_10:
                                return "16 BY 10 WIDE";
                            case starfox::simulation::DisplayMode::ultrawide_21_9:
                                return "21 BY 9 ULTRA";
                            case starfox::simulation::DisplayMode::super_ultrawide_32_9:
                                return "32 BY 9 SUPER";
                            case starfox::simulation::DisplayMode::standard_4_3:
                            default:
                                return "4 BY 3 STANDARD";
                            }
                        }();
                        const auto experience = game.experience()
                            == starfox::simulation::Experience::original
                            ? std::string_view{"ORIGINAL"}
                            : std::string_view{"STARFOX EX"};
                        const auto visual_order = starfox::simulation::pregame_menu_order(game.pregame_page());
                        const bool main_page = game.pregame_page() == starfox::simulation::PregamePage::main;
                        if (!main_page) draw_centred(game.pregame_page() == starfox::simulation::PregamePage::two_d
                            ? "2D OPTIONS" : "3D OPTIONS", 27, 10U);
                        std::array<std::int32_t, 30> row_y;
                        row_y.fill(-1);
                        for (unsigned row = 0; row < visual_order.size(); ++row) {
                            row_y[visual_order[row]] = main_page ? 28 + row * 14
                                : (visual_order.size() > 12 ? 40 + row * 14
                                    : visual_order.size() > 11 ? 44 + row * 15
                                    : 48 + row * (visual_order.size() > 10 ? 16 : 18));
                        }
                        const auto on_off = [](bool enabled) {
                            return enabled ? std::string_view{"ON"}
                                           : std::string_view{"OFF"};
                        };
                        const auto draw_graphics_row =
                            [&text_renderer, &framebuffer, viewport_origin,
                                menu_label_x, menu_value_right](
                                std::string_view label, std::string_view value,
                                std::int32_t y, bool selected) {
                                if (y < 0) return;
                                const auto colour = static_cast<std::uint8_t>(
                                    selected ? 14U : 7U);
                                text_renderer.draw_ascii(label,
                                    menu_label_x + viewport_origin,
                                    y, framebuffer, colour);
                                if (!value.empty()) {
                                    text_renderer.draw_ascii(value,
                                        menu_value_right
                                            - text_renderer.measure_ascii(value)
                                            + viewport_origin,
                                        y, framebuffer, colour);
                                }
                            };
                        draw_graphics_row("EXPERIENCE", game.runtime_options_open()?"LOCKED":experience, row_y[0],
                            game.pregame_selection() == 0U);
                        draw_graphics_row("PACE/SPEED", timing, row_y[1],
                            game.pregame_selection() == 1U);
                        draw_graphics_row("RENDER FPS", presentation, row_y[2],
                            game.pregame_selection() == 2U);
                        draw_graphics_row("DISPLAY", display, row_y[3],
                            game.pregame_selection() == 3U);
                        draw_graphics_row("RENDERER",
                            game.renderer_mode()
                                    == starfox::simulation::RendererMode::gpu
                                ? std::string_view{"GPU"}
                                : std::string_view{"SOFTWARE"},
                            row_y[4], game.pregame_selection() == 4U);
                        const auto msu1_value = game.msu1_available()
                            ? on_off(game.msu1_music())
                            : std::string_view{"NOT FOUND"};
                        draw_graphics_row("MSU-1 MUSIC", msu1_value,
                            row_y[5], game.pregame_selection() == 5U);
                        draw_graphics_row("RUMBLE", on_off(game.rumble()), row_y[6],
                            game.pregame_selection() == 6U);
                        draw_graphics_row("ANTI-ALIASING",
                            anti_aliasing_name(game.anti_aliasing_mode()), row_y[7],
                            game.pregame_selection() == 7U);
                        draw_graphics_row("2D FILTER",
                            two_d_filter_name(game.two_d_filter()), row_y[8],
                            game.pregame_selection() == 8U);
                        draw_graphics_row("RENDER UPSCALE",
                            render_scale_name(game.render_scale()), row_y[9],
                            game.pregame_selection() == 9U);
                        draw_graphics_row("ENHANCED LIGHTING",
                            std::array<std::string_view, 4>{
                                "OFF", "LOW", "MEDIUM", "HIGH"}
                                [game.rtx_lighting_intensity()], row_y[10],
                            game.pregame_selection() == 10U);
                        draw_graphics_row("VSYNC", on_off(game.vsync()), row_y[11],
                            game.pregame_selection() == 11U);
                        draw_graphics_row("MODEL EFFECTS", starfox::render::effect_names[game.effect()], row_y[12],
                            game.pregame_selection() == 12U);
                        draw_graphics_row("WORLD EFFECTS", starfox::render::effect_names[game.world_effect()], row_y[13],
                            game.pregame_selection() == 13U);
                        draw_graphics_row("OPTIONS", "A  OPEN", row_y[14],
                            game.pregame_selection() == 14U);
                        draw_graphics_row(game.runtime_options_open()?"RESUME":"START GAME", "", row_y[15],
                            game.pregame_selection() == 15U);
                        draw_graphics_row("PREVIEW", on_off(game.preview_requested()), row_y[16],
                            game.pregame_selection() == 16U);
                        draw_graphics_row("3D BLOOM", starfox::render::bloom_names[game.bloom()], row_y[17],
                            game.pregame_selection() == 17U);
                        draw_graphics_row("2D BLOOM", starfox::render::bloom_names[game.bloom_2d()], row_y[18],
                            game.pregame_selection() == 18U);
                        draw_graphics_row("3D SMOOTHING", starfox::render::bloom_names[game.model_smoothing()], row_y[19],
                            game.pregame_selection() == 19U);
                        draw_graphics_row("2D OPTIONS", "A  OPEN", row_y[20], game.pregame_selection() == 20U);
                        draw_graphics_row("3D OPTIONS", "A  OPEN", row_y[21], game.pregame_selection() == 21U);
                        draw_graphics_row("MODEL EFFECT INTENSITY", std::to_string(game.effect_intensity()) + "%", row_y[22], game.pregame_selection() == 22U);
                        draw_graphics_row("WORLD EFFECT INTENSITY", std::to_string(game.world_effect_intensity()) + "%", row_y[24], game.pregame_selection() == 24U);
                        draw_graphics_row("BACK", "", row_y[23], game.pregame_selection() == 23U);
                        draw_graphics_row("RAY TRACING", game.ray_tracing()
                            && (game.renderer_mode()!=starfox::simulation::RendererMode::gpu || !dxr_shadows.available())
                                ? std::string_view{"UNAVAILABLE"} : on_off(game.ray_tracing()), row_y[29], game.pregame_selection() == 29U);
                        draw_graphics_row("CHROMATIC ABERRATION", std::array<std::string_view,4>{"OFF","LOW","MEDIUM","HIGH"}[game.chromatic_aberration()], row_y[27], game.pregame_selection() == 27U);
                        draw_graphics_row("HDR EFFECT", std::array<std::string_view,4>{"OFF","LOW","MEDIUM","HIGH"}[game.hdr_effect()], row_y[28], game.pregame_selection() == 28U);
                        draw_cursor(row_y[game.pregame_selection()] + 5);
                    }
                }
            }
            if (game.flow_state() == starfox::simulation::GameFlowState::gameplay
                && !game.meter_state().enabled
                && (game.map().read_native_byte(player_ship_flags_address)
                    & 0x20U) != 0U) {
                // Normal gameplay INIDISP HDMA starts with a 16-scanline
                // forced-blank band. During the launch this hides BG2's
                // unused tile row above the 224x192 Super FX window; exposing
                // it produces the red/white "corrupt top bar" seen by the PC
                // renderer. The meter handoff ends this launch-only mask.
                const auto black = std::find_if(ppu.cgram.begin(), ppu.cgram.end(),
                    [](std::uint16_t colour) { return (colour & 0x7fffU) == 0U; });
                const auto black_index = black == ppu.cgram.end()
                    ? std::uint8_t{} : static_cast<std::uint8_t>(
                        std::distance(ppu.cgram.begin(), black));
                for (std::int32_t y = 0; y < 16; ++y) {
                    for (std::int32_t x = 0;
                         x < static_cast<std::int32_t>(display_width); ++x) {
                        framebuffer.set(x, y, black_index);
                    }
                }
            }
            // Native blackfade is composed through colour_math below. Its
            // BG-only mask preserves the blinking stage OBJ during revival.
            const auto window_wipe = starfox::simulation::interpolate_window_wipe(
                previous_window_wipe, current_window_wipe,
                interpolation_alpha);
            if(test_frames && std::getenv("STARFOX_TEST_SCRAMBLE_WIPE")
                && std::getenv("STARFOX_TRACE_GPU"))
                std::cerr<<"wipe-frame: "<<presented_frames+1U<<" alpha="<<interpolation_alpha
                    <<" active="<<window_wipe.active<<" horizontal="<<window_wipe.horizontal_opening
                    <<" top="<<window_wipe.opening_top<<" bottom="<<window_wipe.opening_bottom<<'\n';
            const auto launch_wipe = game.flow_state()
                    == starfox::simulation::GameFlowState::gameplay
                // ExitBase hands the source colour-window reveal to the
                // first Corneria background ($33) before normal play begins.
                && game.map().background() == 0x33U
                && window_wipe.active;
            auto wipe_has_started_revealing = false;
            if (launch_wipe) {
                for (std::size_t line = 0U;
                     line < window_wipe.left.size(); ++line) {
                    if (window_wipe.left[line] != 16U
                        || window_wipe.right[line] != 239U) {
                        wipe_has_started_revealing = true;
                        break;
                    }
                }
            }
            if (wipe_has_started_revealing) {
                ++launch_wipe_reveal_frames;
            } else if (!launch_wipe) {
                launch_wipe_reveal_frames = 0U;
            }
            live_fps_overlay.clear();
            if (game.show_fps()) {
                const auto fps_text = std::string{"FPS "}
                    + std::to_string(live_fps.fps());
                text_renderer.draw_ascii(
                    fps_text, 0, 0, live_fps_overlay, 1U, 0U);
            }
            exit_confirmation_overlay.clear();
            if (state_slot_window) {
                text_renderer.draw_ascii("SAVE SLOT", 18, 3, exit_confirmation_overlay, 1U, 0U);
                text_renderer.draw_ascii("SLOT " + std::to_string(state_slot), 30, 16,
                    exit_confirmation_overlay, 1U, 0U);
                text_renderer.draw_ascii("ENTER: CLOSE", 8, 29, exit_confirmation_overlay, 1U, 0U);
            }
            if (exit_confirmation) {
                constexpr std::string_view prompt{"EXIT GAME?"};
                text_renderer.draw_ascii(prompt,
                    (static_cast<std::int32_t>(
                         exit_confirmation_overlay.width())
                        - text_renderer.measure_ascii(prompt)) / 2,
                    4, exit_confirmation_overlay, 1U, 0U);
                const auto yes_width = text_renderer.measure_ascii("YES");
                const auto choice_gap = text_renderer.measure_ascii("       ");
                const auto no_width = text_renderer.measure_ascii("NO");
                const auto choices_x =
                    (static_cast<std::int32_t>(
                         exit_confirmation_overlay.width())
                        - yes_width - choice_gap - no_width) / 2;
                text_renderer.draw_ascii("YES",
                    choices_x,
                    22, exit_confirmation_overlay, 1U, 0U);
                text_renderer.draw_ascii("NO",
                    choices_x + yes_width + choice_gap,
                    22, exit_confirmation_overlay, 1U, 0U);
                const auto selected_x = exit_yes_selected
                    ? choices_x
                    : choices_x + yes_width + choice_gap;
                const auto selected_width = text_renderer.measure_ascii(
                    exit_yes_selected ? std::string_view{"YES"}
                                      : std::string_view{"NO"});
                for (auto x = 0; x < selected_width; ++x) {
                    exit_confirmation_overlay.set(
                        selected_x + x, 35, 1U);
                }
            }
            auto base_palette = starfox::render::decode_bgr555_palette(
                game.map().ppu_state().cgram);
            apply_crosshair_tint(base_palette, game.crosshair_colour());
            if (ex_crosshair_strategy_address != 0U) {
                // The EX model marker has no OBJ shading to preserve. In
                // particular GREEN must not fall back to its authored white.
                base_palette[207U] = crosshair_tint(game.crosshair_colour())
                    .value_or(starfox::render::Rgba8{64U, 255U, 64U, 255U});
            }
            auto presentation_brightness = game.map().display_brightness();
            if (wipe_has_started_revealing
                && launch_wipe_reveal_frames > 1U) {
                // ExitBase arms its visible window a few raster phases before
                // its coarse 20 Hz map loop begins FADEUP.  Let the reveal and
                // fade overlap at a physical 60 Hz cadence, as they do on the
                // cartridge, instead of presenting an extra dead-black hold
                // followed by a 0->9 brightness pop on low output rates.
                const auto reveal_phases = static_cast<std::uint32_t>(
                    (launch_wipe_reveal_frames - 1U) * 60U
                    / std::max<std::uint16_t>(game.presentation_fps(), 1U));
                presentation_brightness = static_cast<std::uint8_t>(
                    std::max<std::uint32_t>(presentation_brightness,
                        std::min<std::uint32_t>(15U, reveal_phases * 3U)));
            }
            auto palette = starfox::render::apply_snes_brightness(
                base_palette, presentation_brightness);
            if (hud_editor.active) {
                // Pick neutral editor colours already present in the active
                // cartridge palette. This keeps the static scene's genuine
                // model, portrait, meter, and level hues intact instead of
                // replacing their shared Super FX palette bank.
                const auto editor_background = nearest_palette_index(
                    palette, {48U, 48U, 60U, 255U});
                const auto editor_foreground = nearest_palette_index(
                    palette, {238U, 238U, 242U, 255U});
                draw_hud_editor_chrome(framebuffer, text_renderer,
                    hud_editor, active_hud_layout,
                    game.experience(), game.display_mode(),
                    editor_background, editor_foreground);
            }
            PresentationEffects presentation_effects;
            presentation_effects.chromatic_aberration=game.chromatic_aberration();
            presentation_effects.hdr_effect=game.hdr_effect();
            if (!shadow_mask.empty() || resident_shadow || stereo_resident_shadow[0] || stereo_resident_shadow[1]
                || !stereo_shadow_masks[0].empty() || !stereo_shadow_masks[1].empty()) {
                for(unsigned eye=0;eye<2;++eye) if(!stereo_shadow_masks[eye].empty())
                    presentation_effects.stereo_shadow_masks[eye]=&stereo_shadow_masks[eye];
                for(unsigned eye=0;eye<2;++eye) if(stereo_resident_shadow[eye])
                    presentation_effects.stereo_resident_shadows[eye]=window.stereo_shadow_output(eye);
                if(!shadow_mask.empty()) presentation_effects.shadow_mask=&shadow_mask;
                if(resident_shadow) presentation_effects.resident_shadow=window.shadow_output();
                presentation_effects.shadow_width=superfx_frame.stored_width();
                presentation_effects.shadow_height=superfx_frame.stored_height();
                presentation_effects.shadow_offset_y=scene_offset_y*static_cast<int>(render_scale);
            }
            if (game.in_setup_menu() && !menu_peek) {
                presentation_effects.setup_overlay = &setup_overlay;
                presentation_effects.setup_left = setup_overlay_left;
                presentation_effects.setup_right = setup_overlay_right;
                presentation_effects.setup_brightness = game.menu_preview()
                    ? 15U : game.map().display_brightness();
            }
            presentation_effects.master_brightness = presentation_brightness;
            if (planet_presentation.briefing_layers) {
                presentation_effects.overlay = &planet_overlay;
                presentation_effects.overlay_brightness =
                    planet_presentation.portrait_brightness;
                presentation_effects.text_overlay = &planet_text_overlay;
                // The text shares BG2's source fixed-colour fade with the
                // portraits. Its darker shades come from palette bank 6.
                presentation_effects.text_overlay_brightness =
                    planet_presentation.portrait_brightness;
            }
            presentation_effects.planet = planet_presentation;
            presentation_effects.wipe = window_wipe;
            presentation_effects.colour_math =
                game.colour_math_effect_state();
            presentation_effects.model_surfaces =
                surface_effects
                ? &superfx_surfaces : nullptr;
            presentation_effects.model_surface_y =
                scene_offset_y * static_cast<std::int32_t>(render_scale);
            presentation_effects.background_fixed_white_subtract =
                game.game_over_background_subtract();
            if (presentation_effects.background_fixed_white_subtract != 0U) {
                presentation_effects.fixed_subtract_foreground =
                    &superfx_frame;
                presentation_effects.fixed_subtract_foreground_y =
                    scene_offset_y;
            }
            presentation_effects.expand_wipe = display_width > snes_width
                && extend_cartridge_scene;
            presentation_effects.expand_wipe_vertical = extend_scene_vertical
                || (game.flow_state()
                        == starfox::simulation::GameFlowState::stage_results
                    && window_wipe.active);
            presentation_effects.clip_circle = controls_screen;
            presentation_effects.circle_left = static_cast<std::int16_t>(
                24 + viewport_origin);
            presentation_effects.circle_top = 24;
            presentation_effects.circle_right = static_cast<std::int16_t>(
                136 + viewport_origin);
            presentation_effects.circle_bottom = 112;
            if (game.show_fps()) {
                presentation_effects.host_overlay = &live_fps_overlay;
                presentation_effects.host_overlay_x =
                    static_cast<std::int32_t>(display_width
                        - live_fps_overlay.width() - 4U);
                presentation_effects.host_overlay_y = 4;
            }
            if (exit_confirmation || state_slot_window) {
                presentation_effects.confirmation_overlay =
                    &exit_confirmation_overlay;
            }
            presentation_effects.touch_controls = game.on_screen_controls()
                && touch_controls.visible();
            const auto profile_composite_done =
                std::chrono::steady_clock::now();
            presentation_effects.late_dust=late_dust?&*late_dust:nullptr;
            presentation_effects.late_cartridge=late_cartridge.get();
            presentation_effects.background=deferred_background?&*deferred_background:nullptr;
            presentation_effects.background_cpu_coverage=background_cpu_coverage;
            presentation_effects.temporal_background=temporal_background?&*temporal_background:nullptr;
            presentation_effects.temporal_camera.position={camera.x,camera.y,camera.z};
            for(unsigned i=0;i<9;++i) presentation_effects.temporal_camera.world_to_view[i]=double(view_matrix[i])/32768.;
            if(shadows_enabled && !ppu.tunnel_scene) {
                const auto point=world_to_camera(camera.x,shadow_height,camera.z,camera,view_matrix);
                const auto normal=world_to_camera(camera.x,camera.y+1,camera.z,camera,view_matrix);
                presentation_effects.temporal_ground=starfox::render::TemporalGroundPlane{
                    {float(normal.x),float(normal.y),float(normal.z),float(-(point.x*normal.x+point.y*normal.y+point.z*normal.z))},shadow_height};
            }
            const bool stereo_presented=resident_raster && record_models && game.stereo_output()!=0U
                && window.present_stereo(recorded_scene,framebuffer,palette,circle,presentation_effects,
                    raster_commands,superfx_frame.draw_scale(),resident_layer,game.stereo_output());
            bool temporal_presented=false;
            if(!stereo_presented && resident_raster) {
                const bool replay=record_models && game.stereo_output()!=0U
                    && !window.submit_scene(recorded_scene,superfx_frame.stored_width(),superfx_frame.stored_height());
                if(mono_shadows_deferred) {
                    // Only a failed stereo presentation needs a third, mono
                    // shadow pass. Rebind its result after rebuilding the mono
                    // model batch; never show a prior-frame/left-eye mask here.
                    render_model_shadows(true);
                    presentation_effects.shadow_mask=shadow_mask.empty()?nullptr:&shadow_mask;
                    presentation_effects.resident_shadow=resident_shadow?window.shadow_output():decltype(presentation_effects.resident_shadow){};
                }
                temporal_presented=window.present_native(framebuffer,palette,circle,presentation_effects,
                    raster_commands,superfx_frame.draw_scale(),resident_layer,nullptr,true,replay);
            }
            else if(!stereo_presented) {
                if(deferred_background) restore_background(*deferred_background,framebuffer,framebuffer.write_coverage());
                if(deferred_background && deferred_background->margin_origin && !deferred_background->repair_margins) fill_frontend_margins(framebuffer,deferred_background->margin_origin);
                presentation_effects.background=nullptr;presentation_effects.background_cpu_coverage={};
                window.present(framebuffer, palette, circle, presentation_effects);
            }
            window.finish_temporal_frame(temporal_presented);
            framebuffer.end_write_coverage();
            if (test_frames != 0 && presented_frames + 1U == test_frames) {
                if (const auto* raw_capture = std::getenv("STARFOX_CAPTURE_INDEXED_PATH"))
                    starfox::render::write_bmp(framebuffer, raw_capture, palette);
                if (const auto* layer_capture = std::getenv("STARFOX_CAPTURE_TITLE_LAYERS")) {
                    std::cerr<<"background-state: mode="<<unsigned(ppu.background_mode)
                        <<" bg2_map="<<ppu.bg2_screen_base<<" size="<<unsigned(ppu.bg2_screen_size)
                        <<" chars="<<ppu.bg2_character_base<<" scroll="<<background_x<<','<<background_y
                        <<" hofs="<<ppu.bg2_horizontal_offsets_enabled
                        <<" vofs="<<ppu.bg2_scanline_scroll_enabled<<" tunnel="<<ppu.tunnel_scene
                        <<" rows="<<ppu.bg2_horizontal_offsets[0]<<','<<ppu.bg2_horizontal_offsets[111]
                        <<','<<ppu.bg2_horizontal_offsets[223]<<'\n';
                    for (const auto* name : {"SETBG2VOFS", "XHDMA_BG2VOFS"}) {
                        const auto addresses = symbols.find(name);
                        if (addresses.empty()) continue;
                        std::cerr << "background-native: " << name << ':';
                        for (std::uint32_t byte = 0; byte < 128U; ++byte)
                            std::cerr << ' ' << unsigned(game.map().read_native_byte(addresses.front() + byte));
                        std::cerr << '\n';
                    }
                    auto diagnostic_palette = palette;
                    diagnostic_palette[0] = {255U, 0U, 255U, 255U};
                    starfox::render::Framebuffer diagnostic{display_width, snes_height};
                    background_renderer.draw_bg2(ppu, background_x, background_y,
                        diagnostic, starfox::render::TilePriorityPass::all, viewport_origin, true);
                    starfox::render::write_bmp(diagnostic,
                        std::string{layer_capture} + "-bg2-expanded.bmp", diagnostic_palette);
                    diagnostic.clear();
                    auto unscrolled = ppu;
                    unscrolled.bg2_scanline_scroll_enabled = false;
                    unscrolled.bg2_horizontal_offsets_enabled = false;
                    unscrolled.bg2_vertical_offsets_enabled = false;
                    background_renderer.draw_bg2(unscrolled, 0, 0, diagnostic,
                        starfox::render::TilePriorityPass::all, viewport_origin, false);
                    starfox::render::write_bmp(diagnostic,
                        std::string{layer_capture} + "-bg2-unscrolled.bmp", diagnostic_palette);
                    starfox::render::Framebuffer tilemap_diagnostic{
                        (ppu.bg2_screen_size & 1U) != 0U ? 512U : 256U,
                        (ppu.bg2_screen_size & 2U) != 0U ? 512U : 256U};
                    unscrolled.tunnel_scene = false;
                    background_renderer.draw_bg2(unscrolled, 0, 0, tilemap_diagnostic,
                        starfox::render::TilePriorityPass::all, 0, true);
                    starfox::render::write_bmp(tilemap_diagnostic,
                        std::string{layer_capture} + "-bg2-tilemap.bmp", diagnostic_palette);
                    std::cerr << "background-palette:";
                    for (std::size_t index = 0; index < ppu.cgram.size(); ++index)
                        std::cerr << ' ' << index << '=' << ppu.cgram[index];
                    std::cerr << '\n';
                    for (unsigned layer = 1; layer <= 3; ++layer) {
                        diagnostic.clear();
                        if (layer == 1) background_renderer.draw_bg1(ppu, diagnostic,
                            starfox::render::TilePriorityPass::all, viewport_origin, false);
                        if (layer == 2) background_renderer.draw_bg2(ppu, background_x, background_y,
                            diagnostic, starfox::render::TilePriorityPass::all, viewport_origin, false);
                        if (layer == 3) background_renderer.draw_bg3(ppu, diagnostic,
                            starfox::render::TilePriorityPass::all, viewport_origin, false);
                        starfox::render::write_bmp(diagnostic,
                            std::string{layer_capture} + std::to_string(layer) + ".bmp", diagnostic_palette);
                    }
                }
            }
#if defined(STARFOX_UWP)
            if (uwp_first_runtime_frame) {
                log_uwp_startup("first game/menu frame presented");
                uwp_first_runtime_frame = false;
            }
#endif
            const auto profile_present_done = std::chrono::steady_clock::now();
            if (presented_frames>=profile_warmup) {
            ++profile_measured_frames;
            if (profile_distribution) {
                profile_render_samples.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        profile_present_done - profile_frame_start).count()));
            }
            profile_background_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    profile_background_done - profile_frame_start).count());
            profile_world_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    profile_world_done - profile_background_done).count());
            profile_composite_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    profile_composite_done - profile_world_done).count());
            profile_present_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    profile_present_done - profile_composite_done).count());
            }
            if (presentation_history
                && std::getenv("STARFOX_TEST_DISABLE_HISTORY") == nullptr) {
                presentation_history->record(
                    framebuffer.stored_width(), framebuffer.stored_height(),
                    window.rgba());
            }
            if (advance_frozen_frame) {
                window.set_frame_debug_status(true,
                    presentation_history
                        ? presentation_history->cursor() : 0U,
                    presentation_history
                        ? presentation_history->frame_count() : 0U);
            }
            if (!advance_frozen_frame) {
                live_fps.record_frame(std::chrono::steady_clock::now());
            }
            if (!capture_directory.empty() && presented_frames >= capture_start
                && (presented_frames - capture_start) % capture_interval == 0U) {
                auto name = std::to_string(presented_frames);
                if (name.size() < 6U) name.insert(0U, 6U - name.size(), '0');
                window.save_bmp(capture_directory / (name + ".bmp"));
                if (test_frames != 0 && (std::getenv("STARFOX_TEST_NUCLEUS_DEFEAT") || std::getenv("STARFOX_TEST_PPU_DUMP"))) {
                    const auto& state = game.map().ppu_state();
                    std::ofstream dump(capture_directory / (name + ".vram"), std::ios::binary);
                    dump.write(reinterpret_cast<const char*>(state.vram.data()), state.vram.size());
                    std::ofstream colours(capture_directory / (name + ".cgram"), std::ios::binary);
                    colours.write(reinterpret_cast<const char*>(state.cgram.data()), state.cgram.size() * 2U);
                    std::ofstream offsets(capture_directory / (name + ".offsets"), std::ios::binary);
                    offsets.write(reinterpret_cast<const char*>(state.bg2_horizontal_offsets.data()), state.bg2_horizontal_offsets.size() * 2U);
                    if(std::getenv("STARFOX_TEST_PPU_DUMP")) {
                        std::ofstream vertical(capture_directory/(name+".vertical"),std::ios::binary);
                        vertical.write(reinterpret_cast<const char*>(state.bg2_scanline_scroll_y.data()),state.bg2_scanline_scroll_y.size()*2U);
                        std::ofstream metadata(capture_directory/(name+".ppu.json"));
                        metadata<<"{\"mode\":"<<unsigned(state.background_mode)
                            <<",\"bg2_map\":"<<state.bg2_screen_base<<",\"bg2_size\":"<<unsigned(state.bg2_screen_size)
                            <<",\"bg2_char\":"<<state.bg2_character_base<<",\"bg2_tile16\":"<<state.bg2_tile_size_16
                            <<",\"x\":"<<state.bg2_scroll_x<<",\"y\":"<<state.bg2_scroll_y
                            <<",\"horizontal\":"<<state.bg2_horizontal_offsets_enabled
                            <<",\"vertical\":"<<state.bg2_scanline_scroll_enabled<<"}\n";
                    }
                    std::cerr << "nucleus-frame " << presented_frames
                        << " mode=" << unsigned(state.background_mode)
                        << " bg2chr=" << state.bg2_character_base
                        << " bg2scr=" << state.bg2_screen_base
                        << " bg2tile=" << state.bg2_tile_size_16
                        << " bg=" << game.map().background()
                        << " scroll=" << state.bg2_scroll_x << ',' << state.bg2_scroll_y
                        << " workY=" << game.map().read_native_word(background_y_address)
                        << " baseY=" << game.map().read_native_word(symbols.find("BG2YSCROLL").at(0))
                        << " hostY=" << background_y
                        << " offsets=" << state.bg2_scanline_scroll_enabled
                        << ',' << state.bg2_vertical_offsets_enabled
                        << ',' << state.bg2_horizontal_offsets_enabled
                        << " objs=" << game.objects().active_count() << '\n';
                }
                if (test_frames != 0 && std::getenv("STARFOX_TEST_REVIVAL") != nullptr) {
                    const auto* revival_player=game.objects().is_active(game.player())
                        ? &game.objects().at(game.player()) : nullptr;
                    std::cerr << "revival-frame " << presented_frames
                        << " ticks=" << source_logic_frames
                        << " strat=" << (revival_player?revival_player->strategy_address:0U)
                        << " age=" << (revival_player?int(revival_player->scratch_bytes[0]):-1)
                        << " flags=" << unsigned(game.map().read_native_byte(symbols.find("GAMEFLAGS").at(0)))
                        << " stage=" << game.map().read_native_word(symbols.find("STAGECNT").at(0))
                        << " black=" << unsigned(game.map().read_native_byte(symbols.find("STAYBLACK").at(0)))
                        << " colour=" << game.colour_math_effect_state().active
                        << '/' << unsigned(game.colour_math_effect_state().red)
                        << " circle=" << game.circle_effect_state().active
                        << '/' << game.circle_effect_state().radius
                        << " anim=" << game.map().peek_ram_word(symbols.find("CIRCLEANIM").at(0)).value_or(0)
                        << '\n';
                }
                if (test_frames != 0 && std::getenv("STARFOX_TEST_CLEAR") != nullptr) {
                    const auto clear_state = game.stage_results_state();
                    std::cerr << "clear-frame " << presented_frames
                        << " flow=" << static_cast<unsigned>(game.flow_state())
                        << " tally=" << clear_state.active << " visible=" << clear_state.visible << '\n';
                }
                if (test_frames != 0 && std::getenv("STARFOX_TEST_ENDING") != nullptr) {
                    std::cerr << "ending-frame " << presented_frames
                        << " flow=" << static_cast<unsigned>(game.flow_state())
                        << " bg=" << std::hex << game.map().background()
                        << " mode=" << unsigned(ppu.background_mode)
                        << " chars=" << ppu.bg2_character_base
                        << " screen=" << ppu.bg2_screen_base
                        << " palette0=" << ppu.cgram[0] << std::dec
                        << " scroll=" << background_x << ',' << background_y
                        << " live=" << ppu.bg2_scroll_x << ',' << ppu.bg2_scroll_y
                        << " boss=" << boss_roll << '\n';
                }
            }
            ++presented_frames;
            if(capture_results) {
                capture_results_visible_frames=results.active && results.visible
                    && results.displayed_percentage==results.percentage
                    ?capture_results_visible_frames+1U:0U;
            }
            const bool results_capture_ready=capture_results && capture_results_visible_frames>=60U;
            if (test_frames != 0 && (presented_frames >= test_frames || results_capture_ready)) {
                if(capture_results && !results_capture_ready)
                    throw std::runtime_error("Results capture deadline reached without a visible completed tally");
                if(results_capture_ready) std::cerr<<"results-capture: frame="<<presented_frames
                    <<" percentage="<<unsigned(results.displayed_percentage)<<" visible=1\n";
                if (!profile_render_samples.empty()) {
                    std::sort(profile_render_samples.begin(), profile_render_samples.end());
                    const auto percentile_us = [&](std::size_t percent) {
                        return profile_render_samples[(profile_render_samples.size()-1U)*percent/100U] / 1'000U;
                    };
                    std::cerr << "render-distribution-us median=" << percentile_us(50)
                              << " p95=" << percentile_us(95)
                              << " p99=" << percentile_us(99)
                              << " max=" << profile_render_samples.back()/1'000U << '\n';
                }
                if (std::getenv("STARFOX_TRACE_FPS") != nullptr) {
                    std::cerr << "fps-matrix display="
                              << static_cast<unsigned>(game.display_mode())
                              << " requested=" << game.presentation_fps()
                              << " pace="
                              << static_cast<unsigned>(game.timing_mode())
                              << " measured=" << live_fps.fps()
                              << " frames=" << presented_frames << '\n';
                }
                if (std::getenv("STARFOX_TRACE_PROFILE") != nullptr
                    && profile_measured_frames != 0U) {
                    const auto average_us = [profile_measured_frames](
                                                std::uint64_t total) {
                        return total / profile_measured_frames / 1'000U;
                    };
                    std::cerr << "render-profile-us background="
                              << average_us(profile_background_ns)
                              << " world=" << average_us(profile_world_ns)
                              << " composite=" << average_us(profile_composite_ns)
                              << " present=" << average_us(profile_present_ns)
                              << " bg-cache="
                              << mode2_background_temporal_hits << '/'
                              << mode2_background_exact_hits << '/'
                              << mode2_background_misses
                              << " layer-cache="
                              << cartridge_layer_temporal_hits << '/'
                              << cartridge_layer_misses
                              << " modes=" << profiled_background_modes[0U]
                              << '/' << profiled_background_modes[1U]
                              << '/' << profiled_background_modes[2U]
                              << '/' << profiled_background_modes[3U]
                              << '/' << profiled_background_modes[4U]
                              << '/' << profiled_background_modes[5U]
                              << '/' << profiled_background_modes[6U]
                              << '/' << profiled_background_modes[7U]
                              << " hud=" << profiled_gameplay_hud_frames
                              << " interpolation="
                              << profile_fractional_presentations << '/'
                              << presented_frames
                              << " cuts=" << profile_scene_cuts << '/'
                              << profile_camera_cuts << '/'
                              << profile_raster_cuts
                              << '\n';
                }
                if (std::getenv("STARFOX_TRACE_MSU1") != nullptr) {
                    std::cerr << "msu1 enabled=" << game.msu1_music()
                              << " available=" << game.msu1_available()
                              << " track=" << audio.msu1_track()
                              << " playing=" << audio.msu1_playing() << '\n';
                }
                if (!capture_path.empty()) window.save_bmp(capture_path);
                if (std::getenv("STARFOX_TRACE_RENDER_STATE") != nullptr) {
                    std::cerr << "reticle experience=" << unsigned(active_experience)
                        << " colour=" << unsigned(game.crosshair_colour())
                        << " strategy=" << ex_crosshair_strategy_address
                        << " pixels=" << std::count(framebuffer.pixels().begin(),framebuffer.pixels().end(),207U)
                        << " rgb=" << unsigned(palette[207].r) << ',' << unsigned(palette[207].g) << ',' << unsigned(palette[207].b) << '\n';
                    for (auto h : game.objects().active_handles()) {
                        const auto& o=game.objects().at(h);
                        if(o.strategy_address == ex_crosshair_strategy_address)
                            std::cerr << "sight " << h << " shape=" << o.shape << " flags=" << unsigned(o.strategy_flags[0]) << ',' << unsigned(o.strategy_flags[3]) << "\n";
                    }
                    const auto& trace_ppu = game.map().ppu_state();
                    std::cerr << "render-state flow="
                              << static_cast<unsigned>(game.flow_state())
                              << " mode="
                              << static_cast<unsigned>(trace_ppu.background_mode)
                              << " tm=$" << std::hex
                              << static_cast<unsigned>(trace_ppu.main_screen)
                              << " bg2sc=$" << trace_ppu.bg2_screen_base
                              << " bg2chr=$" << trace_ppu.bg2_character_base
                              << " bg2tile="
                              << (trace_ppu.bg2_tile_size_16 ? 16 : 8)
                              << " bg3sc=$" << trace_ppu.bg3_screen_base
                              << " bg3chr=$" << trace_ppu.bg3_character_base
                              << " bg=" << game.map().background()
                              << std::dec << " scroll=("
                              << trace_ppu.bg2_scroll_x << ','
                              << trace_ppu.bg2_scroll_y << ") vofs="
                              << trace_ppu.bg2_vertical_offsets_enabled
                              << " hofs="
                              << trace_ppu.bg2_horizontal_offsets_enabled
                              << " dots="
                              << static_cast<int>(game.map().dots_mode())
                              << '\n';
                }
                running = false;
            }
        }

        if (!hud_editor_preview) synchronize_ex_save();
        if (hud_editor.active || hud_editor.dragging) save_hud_layout();
        close_gamepads();
        if (restart_runtime) continue;
        return 0;
        }
        return 0;
    } catch (const std::exception& error) {
        const std::string message =
            std::string{"Star Fox Enhanced could not start:\n\n"} + error.what();
        std::cerr << "starfox_pc failed: " << error.what() << '\n';
#if defined(STARFOX_UWP)
        log_uwp_startup(std::string{"FAILED: "} + error.what());
#endif
#if defined(_WIN32) && !defined(STARFOX_UWP)
        // Automated runtime checks must remain headless even when they find a
        // regression; stderr and the non-zero exit status are sufficient and
        // cannot strand modal dialogs on the user's desktop.
        if (std::getenv("STARFOX_TEST_FRAMES") == nullptr) {
            MessageBoxA(nullptr, message.c_str(), "Star Fox Enhanced",
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
        }
#else
        if (std::getenv("STARFOX_TEST_FRAMES") == nullptr) {
            static_cast<void>(SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                "Star Fox Enhanced", message.c_str(), nullptr));
        }
#endif
        return 1;
    }
}
