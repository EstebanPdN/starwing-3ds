#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/render/palette.hpp"
#include "starfox/render/scaled_text_renderer.hpp"
#include "starfox/render/sprite_renderer.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "updater_3ds.hpp"
#include "hardware_profile.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace starfox::platform_3ds {

enum class MenuAction { none, main_menu, memory_dump, launch_rom, check_update, install_update, cancel_update, switch_update_channel };
enum class MenuPage { closed, root, display, gameplay, developer, update, rom_selection, rom_mode, timing };

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
    OptionsMenu3ds(const assets::RomImage& rom, const assets::SymbolMap& symbols,
        bool current_ex,const std::string& current_rom_path,
        HardwareProfile profile = hardware_profile(true));
    [[nodiscard]] MenuPage page() const noexcept { return page_; }
    [[nodiscard]] bool open() const noexcept { return page_ != MenuPage::closed; }
    [[nodiscard]] bool wide() const noexcept { return wide_; }
    [[nodiscard]] bool original_speed() const noexcept { return original_speed_; }
    [[nodiscard]] bool interpolate() const noexcept { return interpolate_; }
    [[nodiscard]] bool top_hud() const noexcept { return top_hud_; }
    [[nodiscard]] unsigned volume() const noexcept { return volume_; }
    [[nodiscard]] bool auto_save() const noexcept { return auto_save_; }
    [[nodiscard]] bool show_fps() const noexcept { return show_fps_; }
    [[nodiscard]] bool overlay_enabled() const noexcept { return overlay_enabled_; }
    [[nodiscard]] bool confirming() const noexcept {
        return confirming_main_menu_ || confirming_version_ || confirming_update_;
    }
    [[nodiscard]] const std::string& selected_rom_path() const noexcept { return selected_rom_path_; }
    [[nodiscard]] bool selected_rom_ex() const noexcept { return selected_rom_ex_; }
    [[nodiscard]] MenuAction touch(int x, int y, bool controls_screen);
    [[nodiscard]] MenuAction navigate(int delta);
    [[nodiscard]] MenuAction activate();
    [[nodiscard]] MenuAction adjust_update_channel() {
        return page_==MenuPage::update&&selection_==0?hit_row(0):MenuAction::none;
    }
    [[nodiscard]] MenuAction activate_confirmation() noexcept;
    void select_confirmation(bool yes) noexcept {
        confirmation_yes_=yes;
    }
    void cancel_confirmation() noexcept {
        confirming_main_menu_=confirming_version_=confirming_update_=false;
    }
    void set_update_status(const UpdateStatus& status) noexcept { update_status_ = status; }
    MenuAction back();
    void page_update_notes(int delta);
    void set_update_notes(const std::string& notes);
    void close() noexcept {
        if(page_==MenuPage::update && (update_status_.state==UpdateState::installing
            || update_status_.state==UpdateState::checking
            || update_status_.state==UpdateState::downloading)) return;
        page_=MenuPage::closed; selection_=0;
        confirming_main_menu_=confirming_version_=confirming_update_=false;
        startup_selection_=false;
    }
    void open_rom_picker();
    void draw_entry(bool map_stars=false, std::uint32_t sky=0U);
    void draw_space_bottom();
    void draw_game_over_bottom();
    void draw_black();
    void draw_options();
    void draw_title_top(const render::Framebuffer& frame,
        const render::Palette256& palette);
    void draw_title_bottom(std::uint64_t time_ms);
    void draw_level_hud(const simulation::GameSimulation& game,
        const render::Palette256& palette, float fps);
    void draw_overlay(const OverlayMetrics& metrics);
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
    unsigned volume_{100};
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
