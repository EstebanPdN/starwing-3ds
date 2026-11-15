#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/render/palette.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include "starfox/render/sprite_renderer.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "updater_3ds.hpp"
#include "hardware_profile.hpp"
#include "presentation_limit.hpp"
#include "bottom_hud_cache.hpp"
#include "bottom_damage.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The Options menu, its saved settings and the frontend's own screens (entry
// buttons, title, level HUD, diagnostic overlay), drawn into pixel buffers that
// game_3ds.cpp presents. Host-tested by tests/menu_layout_test.cpp and
// tests/timing_menu_test.cpp when the private artwork headers are present.

namespace starfox::platform_3ds {

// What the caller has to do after a menu input; none when the menu handled it.
enum class MenuAction { none, main_menu, memory_dump, launch_rom, check_update, install_update, cancel_update, switch_update_channel };
enum class MenuPage { closed, root, display, gameplay, developer, update, rom_selection, rom_mode, timing };

// The values draw_overlay() shows on the diagnostic page: presented FPS, logic
// ticks per second, 95th percentile frame gap, free and total memory in KiB.
struct OverlayMetrics {
    float fps{},logic_hz{},gap_p95_ms{};
    unsigned heap_free_kib{},heap_total_kib{};
    unsigned linear_free_kib{},linear_total_kib{};
    bool new_3ds{},audio_ready{},gpu_bg2{};
    int audio_core{-1};
    const char* build_version{"?"};
    const char* flow{"BOOT"};
};

// Text is read from the same FONT0FON/FONT0WID/FONT0TRN tables that the
// simulation uses; no substitute system font or copied cartridge blob.
class OptionsMenu3ds {
public:
    // `rom` must outlive the menu. Starts from the profile's defaults, then
    // loads the saved settings (options-old3ds.cfg on Old 3DS, else options.cfg).
    OptionsMenu3ds(const assets::RomImage& rom, const assets::SymbolMap& symbols,
        bool current_ex,const std::string& current_rom_path,
        HardwareProfile profile = hardware_profile(true));
    [[nodiscard]] MenuPage page() const noexcept { return page_; }
    [[nodiscard]] bool open() const noexcept { return page_ != MenuPage::closed; }
    // Saved settings: wide (400-pixel) or original aspect, original game speed
    // or a fixed 20 Hz, interpolation, FPS limit (0 = off, 30 or 60), the top
    // HUD, master volume in percent, auto save, the bottom FPS readout, and
    // SELECT toggling the diagnostic overlay.
    [[nodiscard]] bool wide() const noexcept { return wide_; }
    [[nodiscard]] bool original_speed() const noexcept { return original_speed_; }
    [[nodiscard]] bool interpolate() const noexcept { return interpolate_; }
    [[nodiscard]] unsigned fps_limit() const noexcept { return fps_limit_; }
    [[nodiscard]] bool top_hud() const noexcept { return top_hud_; }
    [[nodiscard]] unsigned volume() const noexcept { return volume_; }
    [[nodiscard]] bool auto_save() const noexcept { return auto_save_; }
    [[nodiscard]] bool show_fps() const noexcept { return show_fps_; }
    [[nodiscard]] bool overlay_enabled() const noexcept { return overlay_enabled_; }
    // A YES/NO question is shown: return to the main menu, switch the game
    // version or install an update.
    [[nodiscard]] bool confirming() const noexcept {
        return confirming_main_menu_ || confirming_version_ || confirming_update_;
    }
    // The ROM and edition chosen on the version pages, for launch_rom.
    [[nodiscard]] const std::string& selected_rom_path() const noexcept { return selected_rom_path_; }
    [[nodiscard]] bool selected_rom_ex() const noexcept { return selected_rom_ex_; }
    // A touch at bottom-screen pixel (x, y). While closed, the menu reacts only
    // when controls_screen says its entry buttons are shown.
    [[nodiscard]] MenuAction touch(int x, int y, bool controls_screen);
    // Moves the selection by `delta` rows, wrapping around the page.
    [[nodiscard]] MenuAction navigate(int delta);
    [[nodiscard]] MenuAction activate();
    // Left/right on the update page's channel row: asks to switch the channel.
    [[nodiscard]] MenuAction adjust_update_channel() {
        return page_==MenuPage::update&&selection_==0?hit_row(0):MenuAction::none;
    }
    // Ends the question; returns its action when YES is selected.
    [[nodiscard]] MenuAction activate_confirmation() noexcept;
    void select_confirmation(bool yes) noexcept {
        confirmation_yes_=yes;
    }
    void cancel_confirmation() noexcept {
        confirming_main_menu_=confirming_version_=confirming_update_=false;
    }
    void set_update_status(const UpdateStatus& status) noexcept { update_status_ = status; }
    // Cancels a question or goes up one page (closing from the root). On the
    // update page during a check or download it returns cancel_update; while
    // an update installs it does nothing.
    MenuAction back();
    // Turns the changelog on the update page by `delta` pages.
    void page_update_notes(int delta);
    // Wraps the changelog text to the update page's width and shows page one.
    void set_update_notes(const std::string& notes);
    // Closes the menu, except on the update page while an update is busy.
    void close() noexcept {
        if(page_==MenuPage::update && (update_status_.state==UpdateState::installing
            || update_status_.state==UpdateState::checking
            || update_status_.state==UpdateState::downloading)) return;
        page_=MenuPage::closed; selection_=0;
        confirming_main_menu_=confirming_version_=confirming_update_=false;
        startup_selection_=false;
    }
    // Opens the ROM list to choose the game at startup: choosing an edition
    // then returns launch_rom without a confirmation.
    void open_rom_picker();
    // The draw_ functions repaint top_pixels() and/or bottom_pixels().
    // draw_entry: the OPTIONS and MAIN MENU buttons over the top screen's sky
    // colour `sky` (RGBA8, 0 keeps the previous one).
    void draw_entry(bool map_stars=false, std::uint32_t sky=0U);
    void draw_space_bottom();
    void draw_game_over_bottom();
    void draw_black();
    // The open page on both screens.
    void draw_options();
    // The 400x240 indexed title picture in colour, with the port's credit line.
    void draw_title_top(const render::Framebuffer& frame,
        const render::Palette256& palette);
    // The space starfield with PUSH START blinking in 500 ms phases of time_ms.
    void draw_title_bottom(std::uint64_t time_ms);
    // Brings bottom_pixels() to the level HUD of this state. The picture
    // is retained between calls: only the parts whose sources changed are
    // painted again. Returns whether a source of the picture changed, which
    // is when the loop presents it again.
    bool draw_level_hud(const simulation::GameSimulation& game,
        const render::Palette256& palette, float fps);
    // What draw_level_hud() rewrote in bottom_pixels() since this was last
    // taken: the pixels a presentation of the level HUD has to refresh.
    [[nodiscard]] ScreenDamage take_level_hud_damage() noexcept {
        const auto damage=hud_damage_;
        hud_damage_.clear();
        return damage;
    }
    // The lower screen showed something else since the level HUD was last
    // presented: its next presentation refreshes every pixel.
    void discard_level_hud_presentation() noexcept { hud_damage_.add_everything(); }
    // The diagnostic overlay page on the bottom screen.
    void draw_overlay(const OverlayMetrics& metrics);
    // Row-major RGBA8 pictures (red in the high byte): 400x240 and 320x240.
    [[nodiscard]] const std::vector<std::uint32_t>& top_pixels() const noexcept { return top_; }
    [[nodiscard]] const std::vector<std::uint32_t>& bottom_pixels() const noexcept { return bottom_; }

private:
    void load();
    void save() const;
    void rect(std::vector<std::uint32_t>& target, int width,
        int x, int y, int w, int h, std::uint32_t color);
    void glyph(std::vector<std::uint32_t>& target, int screen_width,
        char character, int x, int y, int scale, std::uint32_t color);
    int text_width(const char* value, int scale=1) const;
    void text(std::vector<std::uint32_t>& target, int screen_width,
        const char* value, int x, int y, int scale, std::uint32_t color);
    void centered(std::vector<std::uint32_t>& target, int screen_width,
        const char* value, int center_x, int y, int scale, std::uint32_t color);
    void button(int x,int y,int w,int h,const char* label,bool selected=false,
        bool enabled=true);
    void plate();
    [[nodiscard]] MenuAction hit_row(int row);
    [[nodiscard]] int row_count() const;
    void scan_roms();
    struct RomEntry { std::string label, path; };
    const assets::RomImage& rom_;
    render::ScaledTextRenderer game_text_;
    render::Framebuffer hud_text_frame_{320U,240U};
    render::Framebuffer portrait_frame_{32U,40U};
    render::Framebuffer hud_status_frame_{256U,224U};
    render::Framebuffer bomb_icon_frame_{8U,8U};
    render::SpriteRenderer hud_sprites_{};
    BottomHudCache hud_cache_{};
    // The meters hud_status_frame_ was drawn with, the area the FPS panel
    // and its label occupy, and the areas rewritten but not yet presented.
    simulation::MeterState hud_status_meter_{};
    ScreenRect hud_fps_area_{};
    // The FPS label and the value it was formatted from.
    std::array<char,24> hud_fps_label_{};
    float hud_fps_value_{};
    bool hud_fps_formatted_{};
    // The box around the message's glyphs on the picture.
    ScreenRect hud_message_area_{};
    ScreenDamage hud_damage_{};
    // Decoded communication portraits, and which one is on the picture.
    struct PortraitFrame {
        std::array<std::uint8_t,32U*40U> pixels{};
        BottomHudCache::Colours colours{};
        std::uint32_t last_use{};
        std::uint8_t frame{};
        bool alternate{},valid{};
    };
    std::array<PortraitFrame,4U> hud_portraits_{};
    int hud_portrait_shown_{-1};
    std::uint32_t hud_portrait_clock_{};
    [[nodiscard]] int decoded_portrait(std::uint8_t frame,bool alternate);
    std::uint32_t bomb_count_address_{};
    std::uint32_t font_glyphs_{}, font_widths_{}, font_translation_{};
    MenuPage page_{MenuPage::closed};
    int selection_{};
    const char* config_path_{"sdmc:/3ds/Starwing/options.cfg"};
    bool original_speed_{}, interpolate_{true};
    bool wide_{true}, top_hud_{true}, auto_save_{false};
    bool show_fps_{false}, overlay_enabled_{false};
    bool confirming_main_menu_{false}, confirming_version_{false}, confirming_update_{false};
    UpdateStatus update_status_{};
    std::vector<std::string> update_lines_;
    unsigned update_notes_page_{};
    bool confirmation_yes_{false}, current_ex_{false};
    unsigned volume_{100}, fps_limit_{};
    std::vector<RomEntry> roms_;
    std::string selected_rom_path_;
    std::string current_rom_path_;
    bool selected_rom_ex_{};
    bool startup_selection_{};
    int rom_scroll_{};
    std::vector<std::uint32_t> top_,bottom_;
    std::uint32_t entry_sky_{0x29384affU};
};

} // namespace starfox::platform_3ds
