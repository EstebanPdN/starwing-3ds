#include "asset_stage.h"
#include "asset_stage.hpp"
#include "audio_3ds.hpp"
#include "controls_3ds_art.hpp"
#include "display_3ds.hpp"
#include "diagnostics_3ds.hpp"
#include "frame_3ds.hpp"
#include "stereo_3ds.hpp"
#include "frame_pacing.hpp"
#include "input_3ds.hpp"
#include "launch_selection.hpp"
#include "screen_transfer.hpp"
#include "options_menu_3ds.hpp"
#include "rom_probe.h"

#include "starfox/assets/rom.hpp"
#include "starfox/assets/bps.hpp"
#include "starfox/input/buttons.hpp"
#include "starfox/render/palette.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "starfox/state/archive.hpp"
#include "starfox/state/container.hpp"

#include <3ds.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <malloc.h>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

// Homebrew launcher builds otherwise inherit libctru's small default stack.
// Asset validation and the simulation constructor use several nested frames.
extern "C" {
std::uint32_t __stacksize__ = 512U * 1024U;
}

namespace {

std::uint32_t ticks_to_microseconds(std::uint64_t ticks) noexcept {
    return static_cast<std::uint32_t>(ticks * 1'000'000ULL
        / SYSCLOCK_ARM11);
}

constexpr const char *app_dir = "sdmc:/3ds/Starwing";
constexpr const char *build_version = STARWING_BUILD_VERSION;
constexpr const char *bundle_path = "sdmc:/3ds/Starwing/Starfox-Assets.BIN";
constexpr const char *save_path = "sdmc:/3ds/Starwing/starfox-ex.srm";
constexpr std::uint32_t autosave_schema = 0x41555301U;
constexpr const char *autosave_original =
    "sdmc:/3ds/Starwing/autosave-original.bin";
constexpr const char *autosave_ex =
    "sdmc:/3ds/Starwing/autosave-ex.bin";

const char* autosave_path(bool ex) noexcept {
    return ex?autosave_ex:autosave_original;
}

bool write_autosave(const char* path,const starfox::assets::RomImage& rom,
    const starfox::simulation::GameSimulation& game,
    const starfox::platform_3ds::Audio3ds& audio) {
    starfox::state::Writer writer;
    writer(game.save_state(),audio.spc().save_state());
    const auto bytes=starfox::state::pack(autosave_schema,
        starfox::assets::crc32(rom.bytes()),writer.bytes());
    const std::string temporary=std::string(path)+".tmp";
    FILE* file=std::fopen(temporary.c_str(),"wb");
    if(!file) return false;
    const bool written=std::fwrite(bytes.data(),1,bytes.size(),file)
        ==bytes.size();
    const bool closed=std::fclose(file)==0;
    if(written&&closed&&std::rename(temporary.c_str(),path)==0) return true;
    std::remove(temporary.c_str());
    return false;
}

bool restore_autosave(const char* path,const starfox::assets::RomImage& rom,
    starfox::simulation::GameSimulation& game,
    starfox::platform_3ds::Audio3ds& audio) {
    FILE* file=std::fopen(path,"rb");
    if(!file) return false;
    const bool size_ok=std::fseek(file,0,SEEK_END)==0;
    const long length=size_ok?std::ftell(file):-1;
    if(length<=0||length>16L*1024L*1024L
        ||std::fseek(file,0,SEEK_SET)!=0) {
        std::fclose(file);return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    const bool read=std::fread(bytes.data(),1,bytes.size(),file)
        ==bytes.size();
    std::fclose(file);
    if(!read) return false;
    try {
        starfox::state::Reader reader{starfox::state::unpack(bytes,
            autosave_schema,starfox::assets::crc32(rom.bytes()))};
        std::vector<std::uint8_t> game_bytes,audio_bytes;
        reader(game_bytes,audio_bytes);
        reader.finish();
        auto restored=game.restored_state(game_bytes);
        audio.spc().load_state(audio_bytes);
        game.swap_state(*restored);
        game.synchronize_apu_output_ports(audio.output_ports());
        return true;
    } catch(const std::exception&) {
        return false;
    }
}

bool present_menu_pixels(const std::vector<std::uint32_t>& pixels,
    gfxScreen_t screen, starfox::platform_3ds::Display3ds& display,
    std::uint8_t brightness=15U) {
    u16 physical_width{},physical_height{};
    auto* output=gfxGetFramebuffer(screen,GFX_LEFT,
        &physical_width,&physical_height);
    const int logical_width=screen==GFX_TOP?400:320;
    if(!output||physical_width!=240U||physical_height!=logical_width) return false;
    brightness=std::min<std::uint8_t>(brightness,15U);
    const auto dim=[brightness](std::uint32_t c)->std::uint32_t {
        if(brightness==15U) return c;
        const auto scale=[brightness](unsigned channel) {
            return channel*brightness/15U;
        };
        return (scale((c>>24U)&255U)<<24U)
            |(scale((c>>16U)&255U)<<16U)
            |(scale((c>>8U)&255U)<<8U)|255U;
    };
    if(screen==GFX_TOP) {
        auto* words=reinterpret_cast<std::uint16_t*>(output);
        for(int y=0;y<240;++y) for(int x=0;x<400;++x) {
            const auto c=dim(pixels[static_cast<std::size_t>(y)*400U+x]);
            words[static_cast<std::size_t>(x)*240U+239U-y]=
                static_cast<std::uint16_t>(((c>>27U)&31U)<<11U
                    |((c>>18U)&63U)<<5U|((c>>11U)&31U));
        }
        GSPGPU_FlushDataCache(output,400U*240U*2U);
        display.note_top_framebuffer(output);
    } else {
        auto* words=reinterpret_cast<std::uint32_t*>(output);
        starfox::platform_3ds::transfer_rgba_screen(
            pixels.data(), words, 320, brightness);
        GSPGPU_FlushDataCache(output,320U*240U*4U);
        display.note_bottom_framebuffer(output);
    }
    gfxScreenSwapBuffers(screen,false);
    return true;
}

void present_frozen_top(std::span<const u8> pixels,
    starfox::platform_3ds::Display3ds& display) {
    constexpr std::size_t top_bytes=400U*240U*2U;
    u16 width{},height{};
    auto* output=gfxGetFramebuffer(GFX_TOP,GFX_LEFT,&width,&height);
    if(output==nullptr||width!=240U||height!=400U
        ||pixels.size()!=top_bytes) return;
    std::memcpy(output,pixels.data(),top_bytes);
    GSPGPU_FlushDataCache(output,top_bytes);
    display.note_top_framebuffer(output);
    gfxScreenSwapBuffers(GFX_TOP,false);
}
Result speedup_init_result = -1;
Result speedup_config_result = -1;

struct PerformanceHistory {
    std::array<std::array<char, 1024>, 180> rows{};
    unsigned count{}, cursor{};
    void append(const char* row) {
        std::snprintf(rows[cursor].data(), rows[cursor].size(), "%s", row);
        cursor = (cursor + 1U) % rows.size();
        count = std::min<unsigned>(count + 1U, rows.size());
    }
    std::string csv() const {
        std::string result = "session_s,flow,fps,logic_hz,frame_ms,game_ms,draw_ms,upload_ms,audio_wait_ms,vblank_ms,work_p95_ms,work_max_ms,gpu_draw_ms,gpu_submit_ms,presented,missed,bg2_pica,bg2_fallback,world_cache_hits,world_cache_misses,heap_free_kib,linear_free_kib,audio_core,quota_percent,phase_ms,preload_ms,preloaded_shapes,audio_packets,audio_late_packets,audio_late_frames,bg2_fragments_mean,background_ms,world_ms,finish_ms,bottom_ms,hud_reused,tick_setup_ms,tick_object_ms,tick_strategy_ms,tick_view_ms,tick_draw_ms,tick_tail_ms,tick_wipe_ms,tick_dialogue_ms,tick_palette_oam_ms,tick_collision_ms,tick_flow_ms,loop_work_us,vblank_wait_us,tick_work_us,renderer_work_us,presentation_work_us,bottom_work_us,bg2_plan_us,gpu_acquire_us,gpu_build_us,cache_flush_us,display_transfer_us,gpu_submit_us,reuse_validation_us,world_cache_validation_us,world_cache_snapshot_us,hud_build_us,frame_reuse_checks,frame_reuse_hits,display_reuse_checks,display_reuse_hits,coalesced_phases,gpu_submissions,gpu_begin_failures,bg2_cache_validation_us,bg2_plan_cache_hits,bg2_plan_cache_misses,ppu_bg_cache_hits,ppu_bg_cache_misses,continue_bottom_reused,bg2_plan_wait_us,bg2_plan_worker_frames,world_prepare_us,world_raster_us,world_composite_us,native_model_us,finalize_us\n";
        for (unsigned n = 0; n < count; ++n)
            result += rows[(cursor + rows.size() - count + n) % rows.size()].data();
        return result;
    }
};

const char* flow_label(starfox::simulation::GameFlowState flow) noexcept {
    using starfox::simulation::GameFlowState;
    switch (flow) {
    case GameFlowState::pregame_menu: return "PREGAME";
    case GameFlowState::title: return "TITLE";
    case GameFlowState::ex_pregame_menu: return "EX MENU";
    case GameFlowState::intro: return "INTRO";
    case GameFlowState::controls_type: return "CONTROLS";
    case GameFlowState::controls_choice: return "CONTROL PICK";
    case GameFlowState::training: return "TRAINING";
    case GameFlowState::planet_select: return "PLANET PICK";
    case GameFlowState::planet_travel: return "TRAVEL";
    case GameFlowState::gameplay: return "GAMEPLAY";
    case GameFlowState::stage_results: return "RESULTS";
    case GameFlowState::game_over: return "GAME OVER";
    case GameFlowState::continue_choice: return "CONTINUE";
    case GameFlowState::credits: return "CREDITS";
    case GameFlowState::finished: return "FINISHED";
    }
    return "UNKNOWN";
}

void prepare_directory() {
    if (mkdir("sdmc:/3ds", 0777) != 0 && errno != EEXIST)
        throw std::runtime_error("Cannot create /3ds on SD");
    if (mkdir(app_dir, 0777) != 0 && errno != EEXIST)
        throw std::runtime_error("Cannot create Starwing on SD");
}

std::vector<std::string> retail_roms() {
    DIR *directory = opendir(app_dir);
    if (directory == nullptr) return {};
    std::vector<std::string> found;
    while (auto *entry = readdir(directory)) {
        if (!starwing_has_rom_extension(entry->d_name)) continue;
        const std::string candidate = std::string(app_dir) + "/" + entry->d_name;
        StarwingRomInfo info{};
        if (starwing_probe_rom(candidate.c_str(), &info)) {
            found.push_back(entry->d_name);
        }
    }
    closedir(directory);
    std::sort(found.begin(), found.end());
    return found;
}

std::string locate_retail_rom() {
    const auto files = retail_roms();
    return files.empty() ? std::string{} : std::string{app_dir} + "/" + files.front();
}

starfox::assets::RuntimeBundlePayload load_assets(
    const std::string& selected_rom) {
    if(!selected_rom.empty()) {
        StarwingRomInfo info{};
        if(!starwing_probe_rom(selected_rom.c_str(),&info))
            throw std::runtime_error("Selected ROM is not a supported clean dump");
        char cache_name[160]{};
        std::snprintf(cache_name,sizeof(cache_name),
            "sdmc:/3ds/Starwing/Starfox-Assets-%08lX.BIN",
            static_cast<unsigned long>(info.crc32));
        if(FILE* cached=std::fopen(cache_name,"rb")) {
            std::fclose(cached);
            try {return starwing_load_runtime_payload(cache_name);}
            catch(const std::exception&) {}
        }
        char message[200]{};
        if(!starwing_prepare_assets(selected_rom.c_str(),cache_name,
                message,sizeof(message)))
            throw std::runtime_error(message);
        return starwing_load_runtime_payload(cache_name);
    }
    // The ordinary first launch has only a ROM on SD. Skip decoding all
    // patch resources twice when the derived companion does not exist yet.
    if (FILE* bundle = std::fopen(bundle_path, "rb")) {
        std::fclose(bundle);
        try {
            return starwing_load_runtime_payload(bundle_path);
        } catch (const std::exception&) {
            // A stale or incomplete companion can be rebuilt from the ROM.
        }
    }
    const auto retail = locate_retail_rom();
    if (retail.empty())
        throw std::runtime_error(
            "Put a clean Star Fox .sfc/.smc in /3ds/Starwing");
    char message[200]{};
    if (starwing_prepare_assets(retail.c_str(), bundle_path,
            message, sizeof(message)) == 0)
        throw std::runtime_error(message);
    return starwing_load_runtime_payload(bundle_path);
}

std::array<std::uint8_t, 65'536> read_ex_save(const char* path) {
    std::array<std::uint8_t, 65'536> save{};
    FILE *file = std::fopen(path, "rb");
    if (file != nullptr) {
        if (std::fread(save.data(), 1, save.size(), file) != save.size())
            save.fill(0U);
        std::fclose(file);
    }
    return save;
}

void write_ex_save(std::span<const std::uint8_t> bytes,const char* path) {
    if (bytes.size() != 65'536U) return;
    const std::string temporary = std::string(path) + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) return;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file)
        == bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (written && closed) std::rename(temporary.c_str(), path);
}

void show_error(const char *message) {
    if (FILE* log = std::fopen("sdmc:/3ds/Starwing/last-error.txt", "wb")) {
        std::fprintf(log, "Starwing 3DS startup/runtime error\n%s\n", message);
        std::fclose(log);
    }
    consoleInit(GFX_TOP, nullptr);
    consoleClear();
    std::printf("STARWING 3DS\n\n%s\n\n", message);
    std::printf("Check /3ds/Starwing on the SD.\n");
    std::printf("Press START to close.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if ((hidKeysDown() & KEY_START) != 0U) break;
        gspWaitForVBlank();
    }
}

struct RunRequest {std::string rom_path; bool ex{};};

std::optional<RunRequest> run_game(bool ex,const std::string& selected_rom,
    bool choose_on_start=false) {
    auto payload = load_assets(selected_rom);
    std::string autosave_file=autosave_path(ex);
    std::string ex_save_file=save_path;
    if(!selected_rom.empty()) {
        StarwingRomInfo info{};
        if(!starwing_probe_rom(selected_rom.c_str(),&info))
            throw std::runtime_error("Selected ROM changed while loading");
        char path[160]{};
        std::snprintf(path,sizeof(path),
            "sdmc:/3ds/Starwing/autosave-%s-%08lX.bin",
            ex?"ex":"original",static_cast<unsigned long>(info.crc32));
        autosave_file=path;
        std::snprintf(path,sizeof(path),
            "sdmc:/3ds/Starwing/starfox-ex-%08lX.srm",
            static_cast<unsigned long>(info.crc32));
        ex_save_file=path;
    }
    auto rom_bytes = ex ? std::move(payload.starfox_ex_rom)
                        : std::move(payload.original_rom);
    auto symbol_text = ex ? std::move(payload.starfox_ex_symbols)
                          : std::move(payload.original_symbols);
    payload = {};
    const starfox::assets::RomImage rom{std::move(rom_bytes)};
    const auto symbols = starfox::assets::SymbolMap::parse(symbol_text);
    symbol_text.clear();
    auto save = ex ? read_ex_save(ex_save_file.c_str())
        : std::array<std::uint8_t, 65'536>{};
    auto owned_game = std::make_unique<starfox::simulation::GameSimulation>(
        rom, symbols, "BOOT", ex ? std::span<const std::uint8_t>{save}
                                 : std::span<const std::uint8_t>{}, true);
    auto& game = *owned_game;
    if (ex) game.set_experience(starfox::simulation::Experience::starfox_ex);
    // The 3DS candidate targets the desktop port's unlocked source cadence.
    // This is still 20 source ticks/s at most, with 60 presentation phases/s.
    game.set_timing_mode(starfox::simulation::TimingMode::unlocked_20_fps);
    game.set_presentation_fps(60U);
    // Cached SNES PPU backgrounds are large. Keep them off the application's
    // 512 KiB stack, which also hosts the audio and display frontends.
    auto owned_renderer = std::make_unique<starfox::platform_3ds::Frame3ds>(
        rom, symbols);
    auto& renderer = *owned_renderer;
    renderer.capture_after_tick(game);
    starfox::render::Framebuffer controls_art_frame{400U,240U};
    starfox::platform_3ds::Display3ds display;
    static_cast<void>(display.open());
    starfox::platform_3ds::Audio3ds audio;
    static_cast<void>(audio.open());
    static_cast<void>(audio.prime_upload_sequence(
        game.map().take_apu_port_writes()));
    game.synchronize_apu_output_ports(audio.output_ports());
    starfox::platform_3ds::Input3ds input;
    auto menu=std::make_unique<starfox::platform_3ds::OptionsMenu3ds>(
        rom,symbols,ex,selected_rom);
    if (choose_on_start) menu->open_rom_picker();
    else if (!selected_rom.empty()) {
        const auto basename = selected_rom.substr(selected_rom.find_last_of('/') + 1U);
        if (!starfox::platform_3ds::write_launch_selection(
            "sdmc:/3ds/Starwing/last-rom.cfg", {basename, ex}))
            throw std::runtime_error("Cannot save the last ROM selection on SD");
    }
    display.set_wide(menu->wide());
    audio.set_master_volume(menu->volume());
    // Preload behind black. Existing scene fades own the level transitions;
    // startup/resume uses only a brief reveal, with no loading text or timer.
    bool preload_new_3ds = false;
    APT_CheckNew3DS(&preload_new_3ds);
    menu->draw_black();
    present_menu_pixels(menu->top_pixels(),GFX_TOP,display);
    present_menu_pixels(menu->bottom_pixels(),GFX_BOTTOM,display);
    audio.stop_playback();
    const auto catalog_start = osGetTime();
    const auto preload_heap = mallinfo();
    const auto heap_capacity = std::uint64_t{envGetHeapSize()};
    const auto heap_used = static_cast<std::uint64_t>(preload_heap.uordblks);
    constexpr auto runtime_reserve = 8ULL * 1024U * 1024U;
    const auto available_for_catalog = heap_capacity > heap_used + runtime_reserve
        ? heap_capacity - heap_used - runtime_reserve : 0ULL;
    const auto catalog_budget = std::min<std::uint64_t>(available_for_catalog,
        (preload_new_3ds ? 12U : 6U) * 1024U * 1024U);
    const auto catalog = renderer.preload_catalog(catalog_budget,
        [](void*, unsigned, unsigned) { return aptMainLoop(); });
    const auto catalog_ms = osGetTime() - catalog_start;
    const bool auto_restored=menu->auto_save()
        &&restore_autosave(autosave_file.c_str(),rom,game,audio);
    if(auto_restored) renderer.capture_after_tick(game);
    bool overlay_visible=false;
    bool diagnostic_chord_was_held=false;
    bool resume_from_options=false;
    enum class MainMenuTransition { none, black_hold, title_fade };
    MainMenuTransition main_menu_transition=MainMenuTransition::none;
    std::uint64_t black_until_ms{},title_fade_begin_ms{};
    bool launch_from_pregame = !auto_restored;
    const std::uint64_t session_start_ms = osGetTime();
    std::uint64_t last_autosave_ms = session_start_ms;
    auto previous_ticks = svcGetSystemTick();
    starfox::platform_3ds::FramePacing pacing{SYSCLOCK_ARM11};
    unsigned save_clock = 0U;
    starfox::platform_3ds::Diagnostics3ds diagnostics;
    PerformanceHistory performance;
    starfox::platform_3ds::DiagnosticSnapshot snapshot{};
    std::array<float, 256> frame_times{};
    unsigned frame_time_count = 0U;
    unsigned dashboard_gpu = 0U, dashboard_fallback = 0U;
    std::uint64_t dashboard_bg2_fragments = 0U;
    std::uint64_t dashboard_audio_ms = 0U, dashboard_phase_us = 0U;
    std::uint64_t dashboard_preload_ms = 0U;
    unsigned dashboard_preloaded_shapes = 0U;
    unsigned dashboard_audio_batches = 0U;
    unsigned dashboard_phases = 0U;
    unsigned dashboard_coalesced_phases = 0U;
    unsigned dashboard_gpu_submissions = 0U;
    unsigned dashboard_gpu_begin_failures = 0U;
    unsigned audio_video_phases = 0U;
    std::vector<starfox::simulation::ApuPortWrite> pending_audio_writes;
    pending_audio_writes.reserve(64U);
    bool last_gpu_bg2 = false;
    std::uint64_t dashboard_start_ms = session_start_ms;
    std::uint64_t dashboard_game_ms = 0U, dashboard_draw_ms = 0U;
    std::uint64_t dashboard_upload_ms = 0U, dashboard_vblank_ms = 0U;
    std::uint64_t dashboard_bottom_ms = 0U;
    unsigned dashboard_hud_reused = 0U;
    unsigned dashboard_continue_bottom_reused = 0U;
    double dashboard_background_ms = 0.0, dashboard_world_ms = 0.0;
    double dashboard_finish_ms = 0.0;
    unsigned dashboard_presented = 0U, dashboard_missed = 0U;
    unsigned dashboard_ticks = 0U;
    std::array<double, 11> dashboard_tick_phases{};
    std::uint64_t dashboard_loop_work_us = 0U;
    std::uint64_t dashboard_vblank_wait_us = 0U;
    std::uint64_t dashboard_tick_work_us = 0U;
    std::uint64_t dashboard_renderer_work_us = 0U;
    std::uint64_t dashboard_presentation_work_us = 0U;
    std::uint64_t dashboard_bottom_work_us = 0U;
    std::uint64_t dashboard_world_prepare_us = 0U;
    std::uint64_t dashboard_world_raster_us = 0U;
    std::uint64_t dashboard_world_composite_us = 0U;
    std::uint64_t dashboard_native_model_us = 0U;
    std::uint64_t dashboard_finalize_us = 0U;
    std::uint64_t dashboard_gpu_plan_us = 0U;
    std::uint64_t dashboard_gpu_plan_wait_us = 0U;
    unsigned dashboard_bg2_plan_worker_frames = 0U;
    std::uint64_t dashboard_gpu_acquire_us = 0U;
    std::uint64_t dashboard_gpu_build_us = 0U;
    std::uint64_t dashboard_cache_flush_us = 0U;
    std::uint64_t dashboard_display_transfer_us = 0U;
    std::uint64_t dashboard_gpu_submit_us = 0U;
    std::uint64_t dashboard_bg2_cache_validation_us = 0U;
    unsigned dashboard_bg2_plan_cache_hits = 0U;
    unsigned dashboard_bg2_plan_cache_misses = 0U;
    std::uint64_t dashboard_reuse_validation_us = 0U;
    unsigned dashboard_frame_reuse_checks = 0U;
    unsigned dashboard_frame_reuse_hits = 0U;
    unsigned dashboard_display_reuse_checks = 0U;
    unsigned dashboard_display_reuse_hits = 0U;
    std::uint64_t dashboard_world_cache_validation_us = 0U;
    std::uint64_t dashboard_world_cache_snapshot_us = 0U;
    std::uint64_t dashboard_hud_build_us = 0U;
    std::uint64_t last_world_cache_hits = 0U;
    std::uint64_t last_world_cache_misses = 0U;
    std::uint64_t last_background_cache_hits = 0U;
    std::uint64_t last_background_cache_misses = 0U;
    std::uint64_t last_audio_packets = 0U;
    std::uint64_t last_audio_late_packets = 0U;
    std::uint64_t last_audio_late_frames = 0U;
    bool last_bottom_hud = false;
    bool last_hud_show_fps = false;
    float last_hud_fps = -1.0F;
    std::uint8_t last_hud_brightness = 255U;
    enum class BottomPresentation { unknown, overlay, game_over, continue_screen, dynamic };
    BottomPresentation bottom_presentation = BottomPresentation::unknown;
    std::uint8_t last_game_over_bottom_brightness = 255U;
    std::uint8_t last_continue_bottom_brightness = 255U;
    std::optional<starfox::platform_3ds::OverlayMetrics>
        last_overlay_metrics;

    std::optional<std::uint64_t> loading_reveal_start;
    const auto reveal_brightness = [&](std::uint8_t brightness) {
        if (brightness == 0U) return brightness;
        const auto now = osGetTime();
        if (!loading_reveal_start) loading_reveal_start = now;
        const auto elapsed = std::min<std::uint64_t>(now - *loading_reveal_start, 120U);
        return static_cast<std::uint8_t>(unsigned(brightness) * elapsed / 120U);
    };

    const auto present_bottom_pixels = [&](
        const std::vector<std::uint32_t>& pixels,
        std::uint8_t brightness = 15U) {
        if (present_menu_pixels(pixels,GFX_BOTTOM,display,reveal_brightness(brightness)))
            bottom_presentation = BottomPresentation::dynamic;
    };

    std::optional<RunRequest> next_game;
    const auto present_new_overlay=[&] {
        last_bottom_hud=false;
        starfox::platform_3ds::OverlayMetrics metrics{};
        metrics.build_version=build_version;
        metrics.fps=snapshot.fps;
        metrics.logic_hz=snapshot.logic_hz;
        metrics.heap_free_kib=snapshot.heap_free_kib;
        metrics.heap_total_kib=snapshot.heap_total_kib;
        metrics.linear_free_kib=snapshot.linear_free_kib;
        metrics.linear_total_kib=snapshot.linear_total_kib;
        metrics.new_3ds=snapshot.new_3ds;
        metrics.audio_ready=snapshot.audio_ready;
        metrics.audio_core=snapshot.audio_core;
        metrics.gpu_bg2=last_gpu_bg2;
        metrics.flow=flow_label(game.flow_state());
        const bool unchanged = bottom_presentation
                == BottomPresentation::overlay
            && last_overlay_metrics
            && metrics.build_version == last_overlay_metrics->build_version
            && metrics.fps == last_overlay_metrics->fps
            && metrics.logic_hz == last_overlay_metrics->logic_hz
            && metrics.heap_free_kib == last_overlay_metrics->heap_free_kib
            && metrics.heap_total_kib == last_overlay_metrics->heap_total_kib
            && metrics.linear_free_kib == last_overlay_metrics->linear_free_kib
            && metrics.linear_total_kib == last_overlay_metrics->linear_total_kib
            && metrics.new_3ds == last_overlay_metrics->new_3ds
            && metrics.audio_ready == last_overlay_metrics->audio_ready
            && metrics.audio_core == last_overlay_metrics->audio_core
            && metrics.gpu_bg2 == last_overlay_metrics->gpu_bg2
            && std::strcmp(metrics.flow,last_overlay_metrics->flow) == 0;
        if (unchanged) return;
        menu->draw_overlay(metrics);
        if (present_menu_pixels(menu->bottom_pixels(),GFX_BOTTOM,display)) {
            bottom_presentation = BottomPresentation::overlay;
            last_overlay_metrics = metrics;
        }
    };
    std::vector<u8> frozen_options_top;
    std::uint64_t options_audio_ms=0;
    while (aptMainLoop()) {
        if(menu->open()||menu->confirming()||overlay_visible)
            last_bottom_hud=false;
        input.reserve_select_for_overlay(menu->overlay_enabled());
        input.sample_display_frame();
        const auto keys_down = hidKeysDown();
        const auto keys_held = hidKeysHeld() | keys_down;
        bool capture_requested = false;
        auto capture_kind = diagnostics.poll_shortcut(keys_down,
            keys_held, capture_requested);
        const bool clear_requested = diagnostics.poll_clear_shortcut(
            keys_down, keys_held);
        if (clear_requested) capture_requested = false;
        touchPosition touch{};
        hidTouchRead(&touch);
        const auto flow_before_menu=game.flow_state();
        const bool controls_screen=flow_before_menu
            ==starfox::simulation::GameFlowState::controls_type
            ||flow_before_menu==starfox::simulation::GameFlowState::controls_choice
            ||flow_before_menu==starfox::simulation::GameFlowState::planet_select;
        const bool pause_entry=game.paused()
            &&(flow_before_menu==starfox::simulation::GameFlowState::gameplay
                ||flow_before_menu==starfox::simulation::GameFlowState::training);
        const bool entry_screen=controls_screen||pause_entry;
        if(menu->open()&&pause_entry&&(keys_down&KEY_START)) {
            menu->close();
            input.reset();
            resume_from_options=true;
        }
        const bool diagnostic_chord=(keys_held&(KEY_L|KEY_R|KEY_SELECT))
            ==(KEY_L|KEY_R|KEY_SELECT);
        if(diagnostic_chord&&!diagnostic_chord_was_held)
            overlay_visible=!overlay_visible;
        else if(!diagnostic_chord&&menu->overlay_enabled()
            &&(keys_down&KEY_SELECT))
            overlay_visible=!overlay_visible;
        diagnostic_chord_was_held=diagnostic_chord;
        auto menu_action=starfox::platform_3ds::MenuAction::none;
        const bool was_confirming=menu->confirming();
        if((keys_down&KEY_TOUCH)&&!resume_from_options&&!overlay_visible)
            menu_action=menu->touch(touch.px,touch.py,entry_screen);
        if(was_confirming&&!overlay_visible) {
            if(keys_down&KEY_B) menu->cancel_confirmation();
            else {
                if(keys_down&KEY_DLEFT) menu->select_confirmation(true);
                if(keys_down&KEY_DRIGHT) menu->select_confirmation(false);
                if(keys_down&KEY_A)
                    menu_action=menu->activate_confirmation();
            }
        }
        const bool menu_was_open=menu->open();
        if(menu->open()&&!was_confirming&&!overlay_visible) {
            if(keys_down&KEY_DUP) static_cast<void>(menu->navigate(-1));
            if(keys_down&KEY_DDOWN) static_cast<void>(menu->navigate(1));
            if(keys_down&KEY_B) menu->back();
            else if(keys_down&KEY_A) menu_action=menu->activate();
        }
        if(menu_was_open&&!menu->open()) {
            input.suppress_until_release(keys_held | keys_down);
            previous_ticks = svcGetSystemTick(); pacing.reset();
        }
        if(menu_action==starfox::platform_3ds::MenuAction::main_menu) {
            audio.stop_playback();
            pending_audio_writes.clear();
            menu->close();
            input.reset();
            overlay_visible=false;
            std::remove(autosave_file.c_str());
            main_menu_transition=MainMenuTransition::black_hold;
            black_until_ms=osGetTime()+900U;
        } else if(menu_action==starfox::platform_3ds::MenuAction::memory_dump) {
            capture_requested=true;
            capture_kind=starfox::platform_3ds::DiagnosticKind3ds::full;
            menu->back();menu->back();
        } else if(menu_action==starfox::platform_3ds::MenuAction::launch_rom) {
            next_game=RunRequest{menu->selected_rom_path(),menu->selected_rom_ex()};
            audio.stop_playback();
            // A short fade hides the following ROM/model preparation without
            // introducing a loading screen or a fixed black hold.
            for (int brightness = 12; brightness >= 0 && aptMainLoop(); brightness -= 3) {
                present_menu_pixels(menu->top_pixels(),GFX_TOP,display,
                    static_cast<std::uint8_t>(brightness));
                present_menu_pixels(menu->bottom_pixels(),GFX_BOTTOM,display,
                    static_cast<std::uint8_t>(brightness));
                gspWaitForVBlank();
            }
            break;
        }
        display.set_wide(menu->wide());
        audio.set_master_volume(menu->volume());
        renderer.set_top_hud_visible(menu->top_hud());
        if(main_menu_transition==MainMenuTransition::black_hold) {
            if(osGetTime()<black_until_ms) {
                menu->draw_black();
                present_menu_pixels(menu->top_pixels(),GFX_TOP,display);
                if(overlay_visible) present_new_overlay();
                else present_bottom_pixels(menu->bottom_pixels());
                gspWaitForVBlank();
                previous_ticks = svcGetSystemTick();
                pacing.reset();
                continue;
            }
            game.return_to_title_from_menu();
            renderer.capture_after_tick(game);
            input.reset();
            main_menu_transition=MainMenuTransition::title_fade;
            title_fade_begin_ms=osGetTime();
            previous_ticks = svcGetSystemTick();
            pacing.reset();
        }
        if(menu->open()) {
            if(frozen_options_top.empty()&&display.top_framebuffer()!=nullptr) {
                // Hold the exact last presented game/menu image while all
                // directional and face-button input belongs to Options.
                if(display.gpu_active()) C3D_FrameSync();
                constexpr std::size_t top_bytes=400U*240U*2U;
                const auto* source=display.top_framebuffer();
                frozen_options_top.assign(source,source+top_bytes);
            }
            if(options_audio_ms==0) options_audio_ms=osGetTime();
            if(audio.update_ready()) {
                static_cast<void>(audio.finish_update());
                game.synchronize_apu_output_ports(audio.output_ports());
            }
            if(osGetTime()-options_audio_ms>=50U&&!audio.pending()) {
                audio.begin_update(pending_audio_writes);
                pending_audio_writes.clear();
                options_audio_ms=osGetTime();
            }
            input.reset();
            menu->draw_options();
            if(menu->page()==starfox::platform_3ds::MenuPage::rom_selection
                ||menu->page()==starfox::platform_3ds::MenuPage::rom_mode)
                present_menu_pixels(menu->top_pixels(),GFX_TOP,display,reveal_brightness(15U));
            else if(!frozen_options_top.empty())
                present_frozen_top(frozen_options_top,display);
            if(overlay_visible) present_new_overlay();
            else present_bottom_pixels(menu->bottom_pixels());
            if(!capture_requested) {
                gspWaitForVBlank();
                previous_ticks = svcGetSystemTick();
                pacing.reset();
                continue;
            }
        } else { frozen_options_top.clear(); options_audio_ms=0; }
        if(menu->confirming()) {
            input.reset();
            menu->draw_entry();
            if(overlay_visible) present_new_overlay();
            else present_bottom_pixels(menu->bottom_pixels());
            if(!capture_requested) {
                gspWaitForVBlank();
                previous_ticks = svcGetSystemTick();
                pacing.reset();
                continue;
            }
        }
        if (overlay_visible && !clear_requested
            && diagnostics.poll_touch(keys_down, touch, 8, 181, 96, 50)) {
            capture_requested = true;
            capture_kind = starfox::platform_3ds::DiagnosticKind3ds::quick;
        } else if (overlay_visible && !clear_requested
            && diagnostics.poll_touch(keys_down, touch, 112, 181, 96, 50)) {
            capture_requested = true;
            capture_kind = starfox::platform_3ds::DiagnosticKind3ds::full;
        }
        const auto loop_begin = svcGetSystemTick();
        const auto now_ticks = svcGetSystemTick();
        pacing.advance(now_ticks - previous_ticks);
        previous_ticks = now_ticks;
        unsigned video_phases = 0U;
        bool logic_advanced = false;
        bool presentation_changed = false;
        while (pacing.ready() && video_phases < 8U
            &&!menu->open()&&!menu->confirming()) {
            const auto phase_begin_tick = svcGetSystemTick();
            game.present_frame();
            dashboard_phase_us += ticks_to_microseconds(
                svcGetSystemTick() - phase_begin_tick);
            pacing.consume();
            ++video_phases;
            ++dashboard_phases;
            if (game.logic_tick_ready()) {
                // A previous audio job must publish its APU ports before the
                // next source tick can read them. Usually the job already
                // completed after the preceding video render below.
                if (audio.pending()) {
                    const auto audio_wait_begin = osGetTime();
                    static_cast<void>(audio.finish_update());
                    dashboard_audio_ms += osGetTime() - audio_wait_begin;
                    game.synchronize_apu_output_ports(audio.output_ports());
                }
                auto controls = input.consume_game_tick();
                if(resume_from_options&&game.paused()) {
                    controls.held|=starfox::input::start;
                    controls.pressed|=starfox::input::start;
                    resume_from_options=false;
                }
                if (launch_from_pregame
                    && game.flow_state()
                        == starfox::simulation::GameFlowState::pregame_menu) {
                    controls.held |= starfox::input::start;
                    controls.pressed |= starfox::input::start;
                    launch_from_pregame = false;
                }
                const auto dashboard_tick_start = osGetTime();
                const auto dashboard_tick_start_tick = svcGetSystemTick();
                const auto previous_flow = game.flow_state();
                const auto result = game.tick(controls);
                for (unsigned n=0;n<dashboard_tick_phases.size();++n)
                    dashboard_tick_phases[n] += game.tick_phase_ms()[n];
                renderer.capture_after_tick(game);
                dashboard_game_ms += osGetTime() - dashboard_tick_start;
                dashboard_tick_work_us += ticks_to_microseconds(
                    svcGetSystemTick() - dashboard_tick_start_tick);
                ++dashboard_ticks;
                logic_advanced = true;
                presentation_changed = presentation_changed
                    || !result.presentation_unchanged;
                pending_audio_writes.insert(pending_audio_writes.end(),
                    result.audio_port_writes.begin(),
                    result.audio_port_writes.end());
                const auto flow = game.flow_state();
                if (flow != previous_flow
                    && (flow == starfox::simulation::GameFlowState::gameplay
                        || flow == starfox::simulation::GameFlowState::training)) {
                    // The source has already reached its black handoff. Show
                    // that frame now and warm only active stage shapes before
                    // the first gameplay render can expose a decode hitch.
                    audio.stop_playback();
                    menu->draw_black();
                    present_menu_pixels(menu->top_pixels(),GFX_TOP,display);
                    present_bottom_pixels(menu->bottom_pixels());
                    gspWaitForVBlank();
                    const auto preload_begin = osGetTime();
                    dashboard_preloaded_shapes +=
                        renderer.preload_active_shapes(game);
                    dashboard_preload_ms += osGetTime() - preload_begin;
                    previous_ticks = svcGetSystemTick();
                    pacing.reset();
                }
                if (ex && ++save_clock >= 20U) {
                    save_clock = 0U;
                    const auto current = game.ex_save_ram();
                    if (current.size() == save.size()
                        && !std::equal(current.begin(), current.end(),
                            save.begin())) {
                        write_ex_save(current,ex_save_file.c_str());
                        std::copy(current.begin(), current.end(), save.begin());
                    }
                }
            }
            // The SPC produces one 50 ms packet per three video phases. It
            // must keep its own 20 Hz cadence when source logic deliberately
            // slows or a render misses a display deadline; APU writes from
            // intervening game ticks are applied to this packet in order.
            if (++audio_video_phases >= 3U) {
                if (audio.pending()) {
                    const auto audio_wait_begin = osGetTime();
                    static_cast<void>(audio.finish_update());
                    dashboard_audio_ms += osGetTime() - audio_wait_begin;
                    game.synchronize_apu_output_ports(audio.output_ports());
                }
                const auto audio_begin = osGetTime();
                audio.begin_update(pending_audio_writes);
                dashboard_audio_ms += osGetTime() - audio_begin;
                pending_audio_writes.clear();
                audio_video_phases -= 3U;
                ++dashboard_audio_batches;
            }
        }
        if (video_phases != 0U) {
            dashboard_coalesced_phases += video_phases - 1U;
            const auto dashboard_render_tick = svcGetSystemTick();
            const auto dashboard_draw_start = osGetTime();
            const auto flow = game.flow_state();
            const bool gpu_bg2 = (flow == starfox::simulation::GameFlowState::gameplay
                || flow == starfox::simulation::GameFlowState::training)
                && menu->wide()
                && display.eligible_mode2(game.map().ppu_state());
            last_gpu_bg2 = gpu_bg2;
            const bool stereo_scene = menu->wide() && display.gpu_active()
                && game.map().ppu_state().background_mode == 2U
                && (flow == starfox::simulation::GameFlowState::gameplay
                    || flow == starfox::simulation::GameFlowState::training);
            renderer.set_stereo_strength(stereo_scene
                ? starfox::platform_3ds::stereo_slider_strength(osGet3DSliderState()) : 0U);
            const bool reusable_flow = flow == starfox::simulation::GameFlowState::title
                || flow == starfox::simulation::GameFlowState::intro
                || flow == starfox::simulation::GameFlowState::controls_type
                || flow == starfox::simulation::GameFlowState::controls_choice
                || flow == starfox::simulation::GameFlowState::planet_select
                || flow == starfox::simulation::GameFlowState::game_over
                || flow == starfox::simulation::GameFlowState::continue_choice
                || flow == starfox::simulation::GameFlowState::gameplay
                || flow == starfox::simulation::GameFlowState::training;
            const bool static_menu = flow == starfox::simulation::GameFlowState::controls_type
                || flow == starfox::simulation::GameFlowState::controls_choice
                || flow == starfox::simulation::GameFlowState::planet_select
                || flow == starfox::simulation::GameFlowState::game_over
                || flow == starfox::simulation::GameFlowState::continue_choice;
            const double interpolation_alpha = game.paused() || static_menu ? 1.0
                : game.logic_interpolation_alpha(0.0);
            bool reuse = false;
            if (reusable_flow && !presentation_changed) {
                ++dashboard_frame_reuse_checks;
                const auto reuse_validation_begin = svcGetSystemTick();
                reuse = renderer.can_reuse_presentation(game, gpu_bg2,
                    interpolation_alpha);
                dashboard_reuse_validation_us += ticks_to_microseconds(
                    svcGetSystemTick() - reuse_validation_begin);
                if (reuse) ++dashboard_frame_reuse_hits;
            }
            if (gpu_bg2) {
                const auto [plan_scroll_x, plan_scroll_y] =
                    renderer.current_background_scroll(game);
                display.prepare_mode2_plan(game.map().ppu_state(),
                    game.map().ppu_vram_revision(), plan_scroll_x,
                    plan_scroll_y);
            }
            const auto& indexed = reuse ? renderer.last_frame()
                : renderer.draw(game, gpu_bg2, interpolation_alpha);
            if (!reuse)
                dashboard_world_cache_validation_us +=
                    renderer.world_cache_validation_us();
            if (!reuse)
                dashboard_world_cache_snapshot_us +=
                    renderer.world_cache_snapshot_us();
            const auto add_draw_breakdown = [&] {
                const auto times = renderer.last_draw_breakdown();
                dashboard_background_ms += times.background_ms;
                dashboard_world_ms += times.world_ms;
                dashboard_finish_ms += times.finish_ms;
                dashboard_world_prepare_us += times.world_prepare_us;
                dashboard_world_raster_us += times.world_raster_us;
                dashboard_world_composite_us += times.world_composite_us;
                dashboard_native_model_us += times.native_model_us;
                dashboard_finalize_us += times.finalize_us;
            };
            const auto record_display_timing = [&] {
                const auto& timing = display.last_frame_timing();
                dashboard_gpu_plan_us += timing.plan_us;
                dashboard_gpu_plan_wait_us += timing.plan_wait_us;
                if (timing.bg2_plan_worker_used)
                    ++dashboard_bg2_plan_worker_frames;
                dashboard_bg2_cache_validation_us +=
                    timing.cache_validation_us;
                dashboard_gpu_acquire_us += timing.acquire_us;
                dashboard_gpu_build_us += timing.build_us;
                dashboard_cache_flush_us += timing.flush_us;
                dashboard_display_transfer_us += timing.transfer_us;
                dashboard_gpu_submit_us += timing.submit_us;
                if (timing.bg2_plan_checked) {
                    if (timing.plan_cache_hit)
                        ++dashboard_bg2_plan_cache_hits;
                    else
                        ++dashboard_bg2_plan_cache_misses;
                }
                if (timing.gpu_begin_failed)
                    ++dashboard_gpu_begin_failures;
                if (timing.gpu_submitted)
                    ++dashboard_gpu_submissions;
            };
            if (!reuse) add_draw_breakdown();
            dashboard_draw_ms += osGetTime() - dashboard_draw_start;
            dashboard_renderer_work_us += ticks_to_microseconds(
                svcGetSystemTick() - dashboard_render_tick);
            const auto dashboard_upload_start = osGetTime();
            const auto dashboard_presentation_tick = svcGetSystemTick();
            std::uint64_t fallback_draw_ms = 0U;
            auto palette = starfox::render::apply_snes_brightness(
                starfox::render::decode_bgr555_palette(
                    game.map().ppu_state().cgram),
                reveal_brightness(game.map().display_brightness()));
            std::uint8_t title_fade=15U;
            if(main_menu_transition==MainMenuTransition::title_fade) {
                const auto fade_elapsed=osGetTime()-title_fade_begin_ms;
                if(fade_elapsed>=350U)
                    main_menu_transition=MainMenuTransition::none;
                else title_fade=static_cast<std::uint8_t>(
                    std::min<std::uint64_t>(15U,fade_elapsed*15U/350U));
            }
            const auto [scroll_x, scroll_y] = reuse
                ? renderer.current_background_scroll(game)
                : std::pair{renderer.background_scroll_x(),
                    renderer.background_scroll_y()};
            const bool controls_flow = flow
                    == starfox::simulation::GameFlowState::controls_type
                || flow == starfox::simulation::GameFlowState::controls_choice;
            const bool controls_ready = controls_flow && game.map().display_brightness()>0U && [&] {
                if(indexed.width()!=400U||indexed.height()!=240U) return false;
                const auto backdrop=indexed.get(200,10);
                unsigned detail_pixels{};
                for(int y=40;y<177;y+=2) for(int x=260;x<360;x+=2) {
                    const auto colour=indexed.get(x,y);
                    if(colour!=backdrop && colour!=0U
                        && (palette[colour].r+palette[colour].g+palette[colour].b)>30
                        && ++detail_pixels>=32U) return true;
                }
                return false;
            }();
            const bool controls_art = controls_ready
                && indexed.width() == 400U && indexed.height() == 240U
                && indexed.draw_scale() == 1U
                && indexed.pixels().size()
                    == controls_art_frame.pixels().size();
            const auto* presented_frame=&indexed;
            if (controls_art) {
                std::copy(indexed.pixels().begin(),indexed.pixels().end(),
                    controls_art_frame.pixels().begin());
                starfox::platform_3ds::draw_controls_3ds_art(
                    controls_art_frame,palette,game.map().display_brightness());
                presented_frame=&controls_art_frame;
            }
            bool presented{};
            bool display_reuse_checked = false;
            if(flow==starfox::simulation::GameFlowState::title) {
                menu->draw_title_top(indexed,palette);
                present_menu_pixels(menu->top_pixels(),GFX_TOP,display,title_fade);
                presented=true;
            } else {
                display_reuse_checked = !gpu_bg2 && reuse && !controls_art;
                if (display_reuse_checked) ++dashboard_display_reuse_checks;
                presented = gpu_bg2
                    ? display.present_mode2(game.map().ppu_state(),
                        game.map().ppu_vram_revision(), scroll_x, scroll_y,
                        indexed, palette, reuse,
                        renderer.stereo_active() ? &renderer.right_frame() : nullptr)
                    : display.present(*presented_frame, palette,
                        reuse&&!controls_art,
                        renderer.stereo_active() ? &renderer.right_frame() : nullptr);
                if (display_reuse_checked
                    && display.last_frame_timing().reuse_hit)
                    ++dashboard_display_reuse_hits;
                record_display_timing();
            }
            if (gpu_bg2 && !presented) {
                // A tile-atlas limit or temporary PICA failure must not
                // strand a foreground-only frame on the top screen.
                C3D_FrameSync();
                ++dashboard_fallback;
                last_gpu_bg2 = false;
                const auto fallback_draw_start = osGetTime();
                const auto& fallback_frame = renderer.draw(game, false,
                    interpolation_alpha);
                fallback_draw_ms = osGetTime() - fallback_draw_start;
                dashboard_draw_ms += fallback_draw_ms;
                add_draw_breakdown();
                presented = display.present(fallback_frame, palette, false,
                    renderer.stereo_active() ? &renderer.right_frame() : nullptr);
                record_display_timing();
            }
            dashboard_presentation_work_us += ticks_to_microseconds(
                svcGetSystemTick() - dashboard_presentation_tick);
            dashboard_upload_ms += osGetTime() - dashboard_upload_start
                - fallback_draw_ms;
            if (presented) {
                ++dashboard_presented;
                if (last_gpu_bg2) {
                    ++dashboard_gpu;
                    dashboard_bg2_fragments += display.bg2_fragment_count();
                }
                const auto bottom_begin=osGetTime();
                const auto bottom_begin_tick=svcGetSystemTick();
                if(overlay_visible) {
                    present_new_overlay();
                } else {
                    bool present_bottom=true;
                    const auto source_brightness=game.map().display_brightness();
                    const auto source_bottom_brightness=flow==starfox::simulation::GameFlowState::title
                        ? static_cast<std::uint8_t>(
                            unsigned(source_brightness)*title_fade/15U)
                        :source_brightness;
                    const auto bottom_brightness=reveal_brightness(source_bottom_brightness);
                    if (flow == starfox::simulation::GameFlowState::title) {
                        last_bottom_hud=false;
                        menu->draw_title_bottom(osGetTime());
                    } else if (flow == starfox::simulation::GameFlowState::controls_type
                    || flow == starfox::simulation::GameFlowState::controls_choice
                    || flow == starfox::simulation::GameFlowState::planet_select
                    || ((flow == starfox::simulation::GameFlowState::gameplay
                            || flow == starfox::simulation::GameFlowState::training)
                            &&game.paused())) {
                        last_bottom_hud=false;
                        // Gameplay may put terrain in the upper corner;
                        // retain the most recent menu sky when paused.
                        const bool source_sky = flow
                            !=starfox::simulation::GameFlowState::gameplay
                            &&flow!=starfox::simulation::GameFlowState::training;
                        const auto& sky_colour = palette[indexed.get(0,0)];
                        const auto sky=source_sky
                            ? (std::uint32_t(sky_colour.r)<<24U)
                                |(std::uint32_t(sky_colour.g)<<16U)
                                |(std::uint32_t(sky_colour.b)<<8U)|255U
                            : 0U;
                        menu->draw_entry(flow
                            ==starfox::simulation::GameFlowState::planet_select,
                            sky);
                    } else if(flow==starfox::simulation::GameFlowState::gameplay
                        ||flow==starfox::simulation::GameFlowState::training) {
                        const auto brightness=bottom_brightness;
                        if(last_bottom_hud&&!logic_advanced
                            &&last_hud_fps==snapshot.fps
                            &&last_hud_brightness==brightness
                            &&last_hud_show_fps==menu->show_fps()) {
                            // The 20 Hz cartridge state is unchanged. Keep
                            // the already displayed bottom framebuffer;
                            // rebuilding and flushing 320x240 pixels on a
                            // duplicate presentation buys no visual change.
                            present_bottom=false;
                            ++dashboard_hud_reused;
                        } else {
                            const auto hud_build_begin = svcGetSystemTick();
                            menu->draw_level_hud(game,palette,snapshot.fps);
                            dashboard_hud_build_us += ticks_to_microseconds(
                                svcGetSystemTick() - hud_build_begin);
                            last_bottom_hud=true;
                            last_hud_fps=snapshot.fps;
                            last_hud_brightness=brightness;
                            last_hud_show_fps=menu->show_fps();
                        }
                    } else if(flow==starfox::simulation::GameFlowState::game_over) {
                        last_bottom_hud=false;
                        if(bottom_presentation == BottomPresentation::game_over
                            && last_game_over_bottom_brightness
                                == bottom_brightness) {
                            present_bottom=false;
                        } else {
                            if(bottom_presentation
                                !=BottomPresentation::game_over)
                                menu->draw_game_over_bottom();
                        }
                    } else if(flow==starfox::simulation::GameFlowState::continue_choice) {
                        last_bottom_hud=false;
                        if(bottom_presentation
                                ==BottomPresentation::continue_screen
                            &&last_continue_bottom_brightness==bottom_brightness) {
                            // CONTINUE uses the fixed starfield from
                            // draw_space_bottom(); reuse its displayed pixels
                            // until the screen brightness or flow changes.
                            present_bottom=false;
                            ++dashboard_continue_bottom_reused;
                        } else {
                            menu->draw_space_bottom();
                        }
                    } else {
                        last_bottom_hud=false;
                        menu->draw_space_bottom();
                    }
                    if(present_bottom) {
                        if (flow == starfox::simulation::GameFlowState::game_over) {
                            if (present_menu_pixels(menu->bottom_pixels(),
                                    GFX_BOTTOM,display,bottom_brightness))
                            {
                                bottom_presentation = BottomPresentation::game_over;
                                last_game_over_bottom_brightness =
                                    bottom_brightness;
                            }
                        } else if(flow
                                ==starfox::simulation::GameFlowState::continue_choice) {
                            if (present_menu_pixels(menu->bottom_pixels(),
                                    GFX_BOTTOM,display,bottom_brightness)) {
                                bottom_presentation =
                                    BottomPresentation::continue_screen;
                                last_continue_bottom_brightness =
                                    bottom_brightness;
                            }
                        } else {
                            present_bottom_pixels(menu->bottom_pixels(),
                                source_bottom_brightness);
                        }
                    }
                }
                dashboard_bottom_ms+=osGetTime()-bottom_begin;
                dashboard_bottom_work_us+=ticks_to_microseconds(
                    svcGetSystemTick()-bottom_begin_tick);
            }
            else ++dashboard_missed;
        }
        // Do not block a 60 Hz video frame while the independent SPC core is
        // still producing its packet. The next game tick waits for the ports
        // when necessary; a completed packet is submitted immediately here.
        if (audio.update_ready()) {
            const auto audio_wait_begin = osGetTime();
            static_cast<void>(audio.finish_update());
            dashboard_audio_ms += osGetTime() - audio_wait_begin;
            game.synchronize_apu_output_ports(audio.output_ports());
        }
        const auto autosave_now=osGetTime();
        const auto autosave_flow=game.flow_state();
        if(menu->auto_save()&&!audio.pending()
            &&pending_audio_writes.empty()
            &&autosave_now-last_autosave_ms>=30'000U
            &&(autosave_flow==starfox::simulation::GameFlowState::gameplay
                ||autosave_flow==starfox::simulation::GameFlowState::training)) {
            static_cast<void>(write_autosave(autosave_file.c_str(),rom,game,audio));
            last_autosave_ms=osGetTime();
            previous_ticks = svcGetSystemTick();
            pacing.reset();
        }
        if (video_phases != 0U && frame_time_count < frame_times.size())
            frame_times[frame_time_count++] = static_cast<float>(
                (svcGetSystemTick() - loop_begin) * (1000.0 / SYSCLOCK_ARM11));
        dashboard_loop_work_us += ticks_to_microseconds(
            svcGetSystemTick() - loop_begin);
        const auto dashboard_vblank_start = osGetTime();
        // Include phase debt and work elapsed since the hardware-clock sample.
        if (pacing.should_wait(svcGetSystemTick() - previous_ticks)) {
            const auto vblank_begin_tick = svcGetSystemTick();
            gspWaitForVBlank();
            dashboard_vblank_wait_us += ticks_to_microseconds(
                svcGetSystemTick() - vblank_begin_tick);
        }
        dashboard_vblank_ms += osGetTime() - dashboard_vblank_start;
        const auto dashboard_now_ms = osGetTime();
        const auto dashboard_elapsed = dashboard_now_ms - dashboard_start_ms;
        if (dashboard_elapsed >= 1000U) {
            const auto attempts = std::max(1U,
                dashboard_presented + dashboard_missed);
            const auto ticks = std::max(1U, dashboard_ticks);
            const auto audio_batches = std::max(1U, dashboard_audio_batches);
            const auto phases = std::max(1U, dashboard_phases);
            const auto bg2_cache_checks = std::max(1U,
                dashboard_bg2_plan_cache_hits
                    + dashboard_bg2_plan_cache_misses);
            const auto heap = mallinfo();
            bool is_new_3ds = false;
            APT_CheckNew3DS(&is_new_3ds);

            snapshot.fps = static_cast<float>(
                1000.0 * dashboard_presented / dashboard_elapsed);
            snapshot.logic_hz = static_cast<float>(
                1000.0 * dashboard_ticks / dashboard_elapsed);
            snapshot.frame_ms = static_cast<float>(dashboard_elapsed) / attempts;
            snapshot.game_ms = static_cast<float>(dashboard_game_ms) / ticks;
            snapshot.draw_ms = static_cast<float>(dashboard_draw_ms) / attempts;
            snapshot.upload_ms = static_cast<float>(dashboard_upload_ms) / attempts;
            snapshot.vblank_ms = static_cast<float>(dashboard_vblank_ms) / attempts;
            snapshot.phase_ms = static_cast<float>(dashboard_phase_us)
                / (1000.0F * phases);
            snapshot.missed = dashboard_missed;
            snapshot.heap_free_kib = static_cast<unsigned>(
                heap.fordblks / 1024U);
            snapshot.heap_total_kib = static_cast<unsigned>(
                heap.arena / 1024U);
            snapshot.linear_free_kib = static_cast<unsigned>(
                linearSpaceFree() / 1024U);
            snapshot.linear_total_kib = static_cast<unsigned>(
                envGetLinearHeapSize() / 1024U);
            snapshot.ppu_mode = static_cast<unsigned>(
                game.map().ppu_state().background_mode);
            snapshot.session_seconds = static_cast<unsigned>(
                (dashboard_now_ms - session_start_ms) / 1000U);
            snapshot.flow = flow_label(game.flow_state());
            snapshot.ex = ex;
            snapshot.gpu_bg2 = last_gpu_bg2;
            snapshot.audio_ready = audio.ready();
            snapshot.audio_async = audio.async_enabled();
            snapshot.new_3ds = is_new_3ds;
            snapshot.audio_core = audio.worker_core();
            std::sort(frame_times.begin(), frame_times.begin() + frame_time_count);
            const float p95 = frame_time_count == 0U ? 0.0F
                : frame_times[(frame_time_count * 95U - 1U) / 100U];
            const float maximum = frame_time_count == 0U ? 0.0F
                : frame_times[frame_time_count - 1U];
            const auto& audio_stats = audio.diagnostics();
            const auto interval_packets = audio_stats.submitted_packets
                - last_audio_packets;
            const auto interval_late_packets = audio_stats.late_packets
                - last_audio_late_packets;
            const auto interval_late_frames = audio_stats.late_frames
                - last_audio_late_frames;
            snapshot.audio_late_packets = static_cast<unsigned>(
                interval_late_packets);
            char row[1024]{};
            std::snprintf(row, sizeof(row),
                "%u,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%llu,%llu,%u,%u,%d,%lu,%.2f,%llu,%u,%llu,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%u\n",
                snapshot.session_seconds, snapshot.flow, snapshot.fps,
                snapshot.logic_hz, snapshot.frame_ms, snapshot.game_ms,
                snapshot.draw_ms, snapshot.upload_ms,
                static_cast<double>(dashboard_audio_ms) / audio_batches,
                snapshot.vblank_ms, p95, maximum,
                C3D_GetDrawingTime(), C3D_GetProcessingTime(),
                dashboard_presented, dashboard_missed, dashboard_gpu,
                dashboard_fallback,
                static_cast<unsigned long long>(renderer.world_cache_hits()
                    - last_world_cache_hits),
                static_cast<unsigned long long>(renderer.world_cache_misses()
                    - last_world_cache_misses),
                snapshot.heap_free_kib,
                snapshot.linear_free_kib, audio.worker_core(),
                static_cast<unsigned long>(audio.cpu_quota_percent()),
                snapshot.phase_ms,
                static_cast<unsigned long long>(dashboard_preload_ms),
                dashboard_preloaded_shapes,
                static_cast<unsigned long long>(interval_packets),
                static_cast<unsigned long long>(interval_late_packets),
                static_cast<unsigned long long>(interval_late_frames),
                static_cast<unsigned long long>(dashboard_gpu == 0U ? 0U
                    : dashboard_bg2_fragments / dashboard_gpu),
                dashboard_background_ms / attempts,
                dashboard_world_ms / attempts,
                dashboard_finish_ms / attempts,
                static_cast<double>(dashboard_bottom_ms) / attempts,
                dashboard_hud_reused);
            const auto tail = std::strlen(row);
            if (tail > 0 && row[tail-1] == '\n')
                std::snprintf(row + tail - 1, sizeof(row) - tail + 1,
                    ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%llu,%u,%u,%llu,%llu,%u,%llu,%u,%llu,%llu,%llu,%llu,%llu\n",
                    dashboard_tick_phases[0]/ticks, dashboard_tick_phases[1]/ticks,
                    dashboard_tick_phases[2]/ticks, dashboard_tick_phases[3]/ticks,
                    dashboard_tick_phases[4]/ticks, dashboard_tick_phases[5]/ticks,
                    dashboard_tick_phases[6]/ticks, dashboard_tick_phases[7]/ticks,
                    dashboard_tick_phases[8]/ticks, dashboard_tick_phases[9]/ticks,
                    dashboard_tick_phases[10]/ticks,
                    static_cast<unsigned long long>(dashboard_loop_work_us / attempts),
                    static_cast<unsigned long long>(dashboard_vblank_wait_us / attempts),
                    static_cast<unsigned long long>(dashboard_tick_work_us / attempts),
                    static_cast<unsigned long long>(dashboard_renderer_work_us / attempts),
                    static_cast<unsigned long long>(dashboard_presentation_work_us / attempts),
                    static_cast<unsigned long long>(dashboard_bottom_work_us / attempts),
                    static_cast<unsigned long long>(dashboard_gpu_plan_us / attempts),
                    static_cast<unsigned long long>(dashboard_gpu_acquire_us / attempts),
                    static_cast<unsigned long long>(dashboard_gpu_build_us / attempts),
                    static_cast<unsigned long long>(dashboard_cache_flush_us / attempts),
                    static_cast<unsigned long long>(dashboard_display_transfer_us / attempts),
                    static_cast<unsigned long long>(dashboard_gpu_submit_us / attempts),
                    static_cast<unsigned long long>(dashboard_reuse_validation_us / attempts),
                    static_cast<unsigned long long>(dashboard_world_cache_validation_us / attempts),
                    static_cast<unsigned long long>(dashboard_world_cache_snapshot_us / attempts),
                    static_cast<unsigned long long>(dashboard_hud_build_us / attempts),
                    dashboard_frame_reuse_checks, dashboard_frame_reuse_hits,
                    dashboard_display_reuse_checks,
                    dashboard_display_reuse_hits,
                    dashboard_coalesced_phases,
                    dashboard_gpu_submissions,
                    dashboard_gpu_begin_failures,
                    static_cast<unsigned long long>(
                        dashboard_bg2_cache_validation_us / bg2_cache_checks),
                    dashboard_bg2_plan_cache_hits,
                    dashboard_bg2_plan_cache_misses,
                    static_cast<unsigned long long>(renderer.background_cache_hits()
                        - last_background_cache_hits),
                    static_cast<unsigned long long>(renderer.background_cache_misses()
                        - last_background_cache_misses),
                    dashboard_continue_bottom_reused,
                    static_cast<unsigned long long>(
                        dashboard_gpu_plan_wait_us / attempts),
                    dashboard_bg2_plan_worker_frames,
                    static_cast<unsigned long long>(
                        dashboard_world_prepare_us / attempts),
                    static_cast<unsigned long long>(
                        dashboard_world_raster_us / attempts),
                    static_cast<unsigned long long>(
                        dashboard_world_composite_us / attempts),
                    static_cast<unsigned long long>(
                        dashboard_native_model_us / attempts),
                    static_cast<unsigned long long>(
                        dashboard_finalize_us / attempts));
            dashboard_tick_phases = {};
            performance.append(row);
            last_world_cache_hits = renderer.world_cache_hits();
            last_world_cache_misses = renderer.world_cache_misses();
            last_background_cache_hits = renderer.background_cache_hits();
            last_background_cache_misses = renderer.background_cache_misses();
            last_audio_packets = audio_stats.submitted_packets;
            last_audio_late_packets = audio_stats.late_packets;
            last_audio_late_frames = audio_stats.late_frames;
            frame_time_count = dashboard_gpu = dashboard_fallback = 0U;
            dashboard_bg2_fragments = 0U;
            dashboard_audio_ms = dashboard_phase_us = 0U;
            dashboard_preload_ms = 0U;
            dashboard_preloaded_shapes = dashboard_audio_batches = 0U;
            dashboard_phases = 0U;
            dashboard_start_ms = dashboard_now_ms;
            dashboard_game_ms = dashboard_draw_ms = 0U;
            dashboard_upload_ms = dashboard_vblank_ms = 0U;
            dashboard_bottom_ms = 0U;
            dashboard_hud_reused = 0U;
            dashboard_continue_bottom_reused = 0U;
            dashboard_background_ms = dashboard_world_ms = 0.0;
            dashboard_finish_ms = 0.0;
            dashboard_presented = dashboard_missed = dashboard_ticks = 0U;
            dashboard_loop_work_us = dashboard_vblank_wait_us = 0U;
            dashboard_tick_work_us = dashboard_renderer_work_us = 0U;
            dashboard_presentation_work_us = dashboard_bottom_work_us = 0U;
            dashboard_world_prepare_us = dashboard_world_raster_us = 0U;
            dashboard_world_composite_us = dashboard_native_model_us = 0U;
            dashboard_finalize_us = 0U;
            dashboard_reuse_validation_us = 0U;
            dashboard_frame_reuse_checks = dashboard_frame_reuse_hits = 0U;
            dashboard_display_reuse_checks = dashboard_display_reuse_hits = 0U;
            dashboard_coalesced_phases = dashboard_gpu_submissions = 0U;
            dashboard_gpu_begin_failures = 0U;
            dashboard_bg2_cache_validation_us = 0U;
            dashboard_bg2_plan_cache_hits = 0U;
            dashboard_bg2_plan_cache_misses = 0U;
            dashboard_world_cache_validation_us = dashboard_world_cache_snapshot_us = 0U;
            dashboard_hud_build_us = 0U;
            dashboard_gpu_plan_us = dashboard_gpu_acquire_us = 0U;
            dashboard_gpu_plan_wait_us = 0U;
            dashboard_bg2_plan_worker_frames = 0U;
            dashboard_gpu_build_us = dashboard_cache_flush_us = 0U;
            dashboard_display_transfer_us = dashboard_gpu_submit_us = 0U;
        }
        if(menu->open()&&capture_requested) {
            // A dump taken inside Options must contain the same two screens
            // the player saw, including the paused upper scene.
            if(display.gpu_active()) C3D_FrameSync();
            menu->draw_options();
            if(menu->page()==starfox::platform_3ds::MenuPage::rom_selection
                ||menu->page()==starfox::platform_3ds::MenuPage::rom_mode)
                present_menu_pixels(menu->top_pixels(),GFX_TOP,display,reveal_brightness(15U));
            else if(!frozen_options_top.empty())
                present_frozen_top(frozen_options_top,display);
            if(overlay_visible) present_new_overlay();
            else present_bottom_pixels(menu->bottom_pixels());
        }
        if (clear_requested) {
            bottom_presentation = BottomPresentation::unknown;
            last_overlay_metrics.reset();
            const auto clear_begin = osGetTime();
            audio.stop_playback();
            static_cast<void>(diagnostics.clear_dumps());
            const auto clear_end = osGetTime();
            dashboard_start_ms += clear_end - clear_begin;
            previous_ticks = svcGetSystemTick();
            pacing.reset();
        } else if (capture_requested) {
            bottom_presentation = BottomPresentation::unknown;
            last_overlay_metrics.reset();
            const auto capture_begin = osGetTime();
            audio.stop_playback();
            // FrameBegin(0) fences the prior command queue. FrameSync alone
            // waits for VBlank and is not a GPU completion fence.
            const bool capture_frame = display.gpu_active() && C3D_FrameBegin(0);
            char runtime[1600]{}, renderer_info[1600]{}, audio_info[1200]{};
            std::snprintf(runtime, sizeof(runtime),
                "{\"build\":\"%s\",\"experience\":\"%s\",\"session_ms\":%llu,"
                "\"flow\":\"%s\",\"new_3ds\":%s,\"speedup_init_result\":%ld,"
                "\"speedup_config_result\":%ld,\"fps\":%.3f,\"logic_hz\":%.3f,"
                "\"timing_mode\":\"%s\",\"audio_ready\":%s,\"audio_core\":%d,\"cpu_quota_percent\":%lu,\"bg2_plan_worker\":%s,\"bg2_plan_worker_core\":%d,"
                "\"stereo_active\":%s,\"stereo_strength\":%u,\"stereo_direction\":\"inward_only\","
                "\"gpu_buffer_flush_bytes\":%zu,\"gpu_buffer_flush_fallbacks\":%lu}\n",
                build_version, ex ? "EX" : "Original",
                static_cast<unsigned long long>(capture_begin - session_start_ms),
                flow_label(game.flow_state()), snapshot.new_3ds ? "true" : "false",
                static_cast<long>(speedup_init_result), static_cast<long>(speedup_config_result),
                snapshot.fps, snapshot.logic_hz,
                game.timing_mode() == starfox::simulation::TimingMode::unlocked_20_fps
                    ? "unlocked_20_hz" : "original_speed",
                audio.ready() ? "true" : "false",
                audio.worker_core(), static_cast<unsigned long>(audio.cpu_quota_percent()),
                display.bg2_plan_worker_enabled() ? "true" : "false",
                display.bg2_plan_worker_enabled() ? 2 : -1,
                renderer.stereo_active() ? "true" : "false", unsigned(renderer.stereo_strength()),
                C2D_GetFrameCacheFlushBytes(), static_cast<unsigned long>(C2D_GetFrameCacheFlushFallbacks()));
            std::snprintf(renderer_info, sizeof(renderer_info),
                "top=400x240\nbottom=320x240 integer 5x7 font\n"
                "source=400x240 native world; 256x224 SNES layers at 72,8\n"
                "pica_active=%u\nbg2_pica=%u\nppu_mode=%u\nhofs=%u\nscanline_y=%u\n"
                "world_and_foreground=CPU indexed raster\nframe_end_cache_flush=used_citro2d_vertices_indices_and_all_command_splits\n"
                "stereo=inward_only; parallel cameras; source convergence=256; HUD and BG2 at screen plane\n"
                "ppu_memory_cache=revision_token\nhud_build_cache=revision_token\n"
                "world_cache_hits=%llu\nworld_cache_misses=%llu\n"
                "ppu_bg_cache_hits=%llu\nppu_bg_cache_misses=%llu\n"
                "top_hud_cache_hits=%llu\ntop_hud_cache_misses=%llu\n"
                "bg2_source_fragments=%zu\nbg2_submitted_fragments=%zu\n"
                "gpu_draw_ms=%.3f\ngpu_submit_ms=%.3f\n",
                display.gpu_active(), last_gpu_bg2, game.map().ppu_state().background_mode,
                game.map().ppu_state().bg2_horizontal_offsets_enabled,
                game.map().ppu_state().bg2_scanline_scroll_enabled,
                static_cast<unsigned long long>(renderer.world_cache_hits()),
                static_cast<unsigned long long>(renderer.world_cache_misses()),
                static_cast<unsigned long long>(renderer.background_cache_hits()),
                static_cast<unsigned long long>(renderer.background_cache_misses()),
                static_cast<unsigned long long>(renderer.top_hud_cache_hits()),
                static_cast<unsigned long long>(renderer.top_hud_cache_misses()),
                display.bg2_source_fragment_count(),
                display.bg2_fragment_count(),
                C3D_GetDrawingTime(), C3D_GetProcessingTime());
            const auto& audio_diagnostics = audio.diagnostics();
            std::snprintf(audio_info, sizeof(audio_info),
                "backend=CSND\nready=%u\nasync=%u\nworker_core=%d\ncpu_quota_percent=%lu\n"
                "csnd_result=%ld\nmodel_result=%ld\ncore2_attempted=%u\ncore2_created=%u\n"
                "core1_attempts=%u\nquota_query_result=%ld\nquota_set_result=%ld\nquota_verify_result=%ld\n"
                "playback_start_result=%ld\nsubmitted_packets=%llu\nlate_packets=%llu\n"
                "late_frames=%llu\nrate_adjust_frames=%llu\ndropped_packets=%llu\nplayback_starts=%llu\n"
                "cache_flush_failures=%llu\nmax_lead_frames=%lu\n"
                "spc_compute_last_ms=%lu\nspc_compute_total_ms=%llu\n"
                "spc_compute_max_ms=%lu\nspc_compute_packets=%lu\n",
                audio.ready(), audio.async_enabled(), audio.worker_core(),
                static_cast<unsigned long>(audio.cpu_quota_percent()),
                static_cast<long>(audio_diagnostics.csnd_result),
                static_cast<long>(audio_diagnostics.model_result),
                audio_diagnostics.core2_attempted, audio_diagnostics.core2_created,
                audio_diagnostics.core1_attempts,
                static_cast<long>(audio_diagnostics.quota_query_result),
                static_cast<long>(audio_diagnostics.quota_set_result),
                static_cast<long>(audio_diagnostics.quota_verify_result),
                static_cast<long>(audio_diagnostics.playback_start_result),
                static_cast<unsigned long long>(audio_diagnostics.submitted_packets),
                static_cast<unsigned long long>(audio_diagnostics.late_packets),
                static_cast<unsigned long long>(audio_diagnostics.late_frames),
                static_cast<unsigned long long>(audio_diagnostics.rate_adjust_frames),
                static_cast<unsigned long long>(audio_diagnostics.dropped_packets),
                static_cast<unsigned long long>(audio_diagnostics.playback_starts),
                static_cast<unsigned long long>(audio_diagnostics.cache_flush_failures),
                static_cast<unsigned long>(audio_diagnostics.max_lead_frames),
                static_cast<unsigned long>(audio_diagnostics.spc_compute_last_ms),
                static_cast<unsigned long long>(audio_diagnostics.spc_compute_total_ms),
                static_cast<unsigned long>(audio_diagnostics.spc_compute_max_ms),
                static_cast<unsigned long>(audio_diagnostics.spc_compute_packets));
            const auto renderer_info_length = std::strlen(renderer_info);
            std::snprintf(renderer_info + renderer_info_length,
                sizeof(renderer_info) - renderer_info_length,
                "catalog_preload_ms=%llu\ncatalog_decoded=%u\ncatalog_candidates=%u\n"
                "catalog_bytes=%llu\ncatalog_limited=%u\ncatalog_rejected=%u\n"
                "catalog_budget=%llu\nheap_capacity=%llu\n"
                "vblank_policy=rational_60hz_hardware_clock_phase_debt\n",
                static_cast<unsigned long long>(catalog_ms), catalog.decoded,
                catalog.candidates, static_cast<unsigned long long>(catalog.bytes),
                unsigned(catalog.limited), catalog.rejected,
                static_cast<unsigned long long>(catalog_budget),
                static_cast<unsigned long long>(heap_capacity));
            const auto csv = performance.csv();
            starfox::platform_3ds::DiagnosticReport3ds report{};
            report.build = "Starwing 3DS hardware candidate 0.67";
            report.top_framebuffer = display.top_framebuffer();
            report.bottom_framebuffer = display.bottom_framebuffer();
            report.runtime_json = runtime;
            report.performance_csv = csv.c_str();
            report.renderer_text = renderer_info;
            report.audio_text = audio_info;
            report.config_text = "target_fps=60\ntiming_mode=unlocked_20_hz\n"
                "render=native400x240_wide_world\naudio=csnd_ring_20hz\n"
                "quick=L+R+A or touch left card\nfull=L+R+B or touch middle card\nclear=L+R+X\n"
                "telemetry=180 seconds RAM ring; no periodic SD logging\ntick_phase_profile=RAM_only\n"
                "dump_time_excluded_from_metrics=yes\nfull_memory_private=yes\n";
            std::vector<u8> top_snapshot;
            std::vector<u8> bottom_snapshot;
            if (capture_kind == starfox::platform_3ds::DiagnosticKind3ds::full
                && report.top_framebuffer && report.bottom_framebuffer
                && gfxGetScreenFormat(GFX_TOP) == GSP_RGB565_OES
                && gfxGetScreenFormat(GFX_BOTTOM) == GSP_RGBA8_OES) {
                constexpr std::size_t top_bytes = 400U * 240U * 2U;
                constexpr std::size_t bottom_bytes = 320U * 240U * 4U;
                if (R_SUCCEEDED(GSPGPU_InvalidateDataCache(
                        report.top_framebuffer, top_bytes))
                    && R_SUCCEEDED(GSPGPU_InvalidateDataCache(
                        report.bottom_framebuffer, bottom_bytes))) {
                    top_snapshot.assign(report.top_framebuffer,
                        report.top_framebuffer + top_bytes);
                    bottom_snapshot.assign(report.bottom_framebuffer,
                        report.bottom_framebuffer + bottom_bytes);
                    report.top_framebuffer = top_snapshot.data();
                    report.bottom_framebuffer = bottom_snapshot.data();
                    report.framebuffer_snapshots = true;
                    report.progress_context = &display;
                    report.progress_callback = [](void* context, const char* stage,
                        u64 written, u64 total) {
                        static_cast<void>(static_cast<starfox::platform_3ds::Display3ds*>(
                            context)->show_full_dump_progress(stage, written, total));
                    };
                    if (display.show_full_dump_progress("PREPARING", 0, 0))
                        gspWaitForVBlank();
                }
            }
            static_cast<void>(diagnostics.capture(capture_kind, report));
            if (capture_frame) C3D_FrameEnd(GX_CMDLIST_FLUSH);
            // Dumps intentionally pause gameplay; exclude all I/O from pacing.
            const auto capture_end = osGetTime();
            dashboard_start_ms += capture_end - capture_begin;
            previous_ticks = svcGetSystemTick();
            pacing.reset();
        }
    }
    if(menu->auto_save()) {
        if(audio.pending()) {
            static_cast<void>(audio.finish_update());
            game.synchronize_apu_output_ports(audio.output_ports());
        }
        if(!pending_audio_writes.empty()) {
            audio.begin_update(pending_audio_writes);
            pending_audio_writes.clear();
            if(audio.pending()) {
                static_cast<void>(audio.finish_update());
                game.synchronize_apu_output_ports(audio.output_ports());
            }
        }
        const auto flow=game.flow_state();
        if(flow==starfox::simulation::GameFlowState::gameplay
            ||flow==starfox::simulation::GameFlowState::training)
            static_cast<void>(write_autosave(autosave_file.c_str(),rom,game,audio));
    }
    if (ex) write_ex_save(game.ex_save_ram(),ex_save_file.c_str());
    return next_game;
}

} // namespace

int main() {
    osSetSpeedupEnable(true);
    speedup_init_result = ptmSysmInit();
    if (R_SUCCEEDED(speedup_init_result)) {
        speedup_config_result = PTMSYSM_ConfigureNew3DSCPU(3);
        ptmSysmExit();
    }
    gfxInitDefault();
    gfxSet3D(false);
    consoleInit(GFX_TOP, nullptr);
    const bool romfs_ready = R_SUCCEEDED(romfsInit());
    try {
        if (!romfs_ready) throw std::runtime_error("Cannot open CIA resources");
        prepare_directory();
        std::remove("sdmc:/3ds/Starwing/last-error.txt");
        const auto files = retail_roms();
        const auto saved = starfox::platform_3ds::read_launch_selection(
            "sdmc:/3ds/Starwing/last-rom.cfg");
        const bool remembered = starfox::platform_3ds::selection_available(saved, files);
        bool ex = remembered && saved.ex;
        bool choose_on_start = !remembered && files.size() > 1U;
        std::string selected_rom = files.empty() ? std::string{}
            : std::string{app_dir} + "/" + (remembered ? saved.filename : files.front());
        while(aptMainLoop()) {
            auto next=run_game(ex,selected_rom,choose_on_start);
            choose_on_start=false;
            if(!next) break;
            selected_rom=std::move(next->rom_path);
            ex=next->ex;
        }
    } catch (const std::exception& error) {
        show_error(error.what());
    }
    if (romfs_ready) romfsExit();
    gfxExit();
    return 0;
}
