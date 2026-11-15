#pragma once
#include "sprite_source_cache.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "starfox/render/palette.hpp"
#include "starfox/render/sprite_renderer.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <tuple>

// BottomHudCache: per-part sources of the bottom-screen HUD picture, so a frame redraws only the
// parts whose sources changed. Used by options_menu_3ds; host-tested by
// tests/bottom_hud_cache_test.cpp.

namespace starfox::platform_3ds {
// The bottom picture is a fixed backdrop and eight parts that never share a
// pixel: the lives readout, the bomb icons, the two meter fills, the
// communication portrait, its health meter, the message and the FPS panel.
// This remembers what each part was drawn from, so that a change in one
// source redraws that part alone. Moving world objects, scrolling BG2 and a
// hidden FPS counter touch none of them. Selected OBJ character bytes are
// compared even when a new simulation resets its revision.
class BottomHudCache {
    static auto dialogue_key(const simulation::DialogueState& d) {
        return std::tuple{d.active,d.text_visible,d.three_lines,d.alternate_portraits,
            d.portrait_frame,d.text_address,d.meter_visible,d.meter_health};
    }
public:
    enum Element : unsigned {
        lives, bombs, shield, boost, portrait, comms_meter, message, fps,
        element_count
    };
    static constexpr unsigned all_elements = (1U << element_count) - 1U;
    [[nodiscard]] static constexpr unsigned bit(Element element) noexcept {
        return 1U << element;
    }

    // The parts drawn from plain values, reduced to exactly those values:
    // two parts with equal sources have equal pixels.
    struct Sources {
        struct Bar {
            std::uint8_t width{};
            std::uint32_t colour{};
            friend constexpr bool operator==(const Bar&, const Bar&) = default;
        };
        Bar shield{}, boost{};
        bool portrait_shown{};
        std::uint8_t portrait_frame{};
        bool alternate_portraits{};
        bool comms_meter_shown{};
        std::uint8_t comms_meter_fill{};
        bool message_shown{};
        std::uint32_t message_address{};
        bool fps_shown{};
        std::array<char, 24> fps_label{};
    };
    // Reduces this frame's meters, dialogue and FPS label to part sources.
    // The 59-pixel sockets fill in proportion to the cartridge meters; the
    // communication meter is 48 pixels for a health of 40.
    [[nodiscard]] static Sources sources(const simulation::MeterState& meter,
        const simulation::DialogueState& dialogue,
        const render::Palette256& palette, bool show_fps, const char* fps_label) noexcept {
        const auto rgba = [&palette](unsigned index) {
            const auto& c = palette[index];
            return (std::uint32_t(c.r) << 24U) | (std::uint32_t(c.g) << 16U)
                | (std::uint32_t(c.b) << 8U) | 255U;
        };
        Sources result;
        const int health_max = meter.extended
            ? std::max(1, int(meter.player_health_max)) : 36;
        result.shield = {static_cast<std::uint8_t>(
                59 * std::clamp(int(meter.damage), 0, health_max) / health_max),
            rgba(meter.shield_up ? 119U : 114U)};
        result.boost = {static_cast<std::uint8_t>(
                59 * std::clamp(int(meter.boost), 0, 36) / 36), rgba(118U)};
        if (dialogue.active) {
            result.portrait_shown = true;
            result.portrait_frame = dialogue.portrait_frame;
            result.alternate_portraits = dialogue.alternate_portraits;
            if (dialogue.meter_visible) {
                result.comms_meter_shown = true;
                result.comms_meter_fill = static_cast<std::uint8_t>(
                    std::min<int>(48, int(dialogue.meter_health) * 48 / 40));
            }
            if (dialogue.text_visible && dialogue.text_address) {
                result.message_shown = true;
                result.message_address = dialogue.text_address;
            }
        }
        if (show_fps) {
            result.fps_shown = true;
            std::strncpy(result.fps_label.data(), fps_label, result.fps_label.size() - 1U);
        }
        return result;
    }

    // One bit per palette entry a part read.
    using Colours = std::array<std::uint32_t, 8U>;
    static constexpr void use(Colours& colours, unsigned index) noexcept {
        colours[index >> 5U] |= 1U << (index & 31U);
    }

    void invalidate() noexcept { valid_ = false; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    struct Stale {
        // The parts whose pixels do not follow from the current sources.
        unsigned elements{};
        // The status OBJ records or their characters changed, so the status
        // layer the lives readout samples must be drawn again.
        bool sprites{};
        // Some source of the picture changed, whether or not a part shows
        // it (a boss meter, a message address while no message is up). The
        // picture then counts as changed and is presented again, on the same
        // frames as a picture redrawn whole on every source change.
        bool changed{};
        // The stale parts that read a palette entry which changed: every
        // pixel of theirs may differ, not only what their sources moved.
        unsigned recoloured{};
    };
    // Compares this frame's sources with the last capture(). An invalid cache
    // reports every part stale and recoloured.
    [[nodiscard]] Stale stale(const simulation::SnesPpuState& ppu, std::uint64_t revision,
        const simulation::MeterState& meter, const simulation::DialogueState& dialogue,
        const Sources& sources, const render::Palette256& palette) {
        if (!valid_) return {all_elements, true, true, all_elements};
        Stale result;
        if (!sprites_.matches(ppu, revision)) {
            result.elements |= bit(lives) | bit(bombs);
            result.sprites = true;
        } else if (!(meter == lives_meter_)) {
            // The readout samples the status layer, which also carries the
            // synthetic meters and the meter-placed ENEMY label. It follows a
            // meter only where one reaches the sampled windows.
            if (lives_reads_meters_ || meter.enabled != lives_meter_.enabled
                || meter.boss_max_health != lives_meter_.boss_max_health) {
                result.elements |= bit(lives);
            } else if (!(meter == lives_clear_meter_)) {
                if (meters_reach_lives(meter)) result.elements |= bit(lives);
                else lives_clear_meter_ = meter;
            }
        }
        if (!(sources.shield == sources_.shield)) result.elements |= bit(shield);
        if (!(sources.boost == sources_.boost)) result.elements |= bit(boost);
        if (sources.portrait_shown != sources_.portrait_shown
            || sources.portrait_frame != sources_.portrait_frame
            || sources.alternate_portraits != sources_.alternate_portraits)
            result.elements |= bit(portrait);
        if (sources.comms_meter_shown != sources_.comms_meter_shown
            || sources.comms_meter_fill != sources_.comms_meter_fill)
            result.elements |= bit(comms_meter);
        if (sources.message_shown != sources_.message_shown
            || sources.message_address != sources_.message_address)
            result.elements |= bit(message);
        if (sources.fps_shown != sources_.fps_shown
            || sources.fps_label != sources_.fps_label) result.elements |= bit(fps);
        if (std::memcmp(palette_.data(), palette.data(), sizeof(palette_)) != 0) {
            for (unsigned element = 0U; element < element_count; ++element) {
                const auto& used = used_[element];
                for (unsigned word = 0U; word < used.size(); ++word) {
                    for (auto bits = used[word]; bits != 0U; bits &= bits - 1U) {
                        const auto index = word * 32U
                            + static_cast<unsigned>(std::countr_zero(bits));
                        if (std::memcmp(&palette_[index], &palette[index],
                                sizeof(palette[index])) != 0) {
                            result.recoloured |= 1U << element;
                            break;
                        }
                    }
                    if ((result.recoloured & (1U << element)) != 0U) break;
                }
            }
            result.elements |= result.recoloured;
        }
        result.changed = result.elements != 0U || !(meter == meter_)
            || dialogue_key(dialogue) != dialogue_key(dialogue_);
        return result;
    }

    // `elements` were drawn from these sources and read the palette entries
    // in `used`. Pass every part stale() reported: the palette is kept whole.
    void capture(unsigned elements, const simulation::SnesPpuState& ppu,
        std::uint64_t revision, const simulation::MeterState& meter,
        const simulation::DialogueState& dialogue,
        const Sources& sources, const render::Palette256& palette,
        const std::array<Colours, element_count>& used) {
        meter_ = meter;
        dialogue_ = dialogue;
        if ((elements & (bit(lives) | bit(bombs))) != 0U) sprites_.capture(ppu, revision);
        if ((elements & bit(lives)) != 0U) {
            lives_meter_ = lives_clear_meter_ = meter;
            capture_lives_windows(ppu);
            lives_reads_meters_ = lives_windows_unbounded_ || meters_reach_lives(meter);
        }
        if ((elements & bit(shield)) != 0U) sources_.shield = sources.shield;
        if ((elements & bit(boost)) != 0U) sources_.boost = sources.boost;
        if ((elements & bit(portrait)) != 0U) {
            sources_.portrait_shown = sources.portrait_shown;
            sources_.portrait_frame = sources.portrait_frame;
            sources_.alternate_portraits = sources.alternate_portraits;
        }
        if ((elements & bit(comms_meter)) != 0U) {
            sources_.comms_meter_shown = sources.comms_meter_shown;
            sources_.comms_meter_fill = sources.comms_meter_fill;
        }
        if ((elements & bit(message)) != 0U) {
            sources_.message_shown = sources.message_shown;
            sources_.message_address = sources.message_address;
        }
        if ((elements & bit(fps)) != 0U) {
            sources_.fps_shown = sources.fps_shown;
            sources_.fps_label = sources.fps_label;
        }
        for (unsigned element = 0U; element < element_count; ++element)
            if ((elements & (1U << element)) != 0U) used_[element] = used[element];
        // A part that was not drawn read none of the entries that differ.
        palette_ = palette;
        valid_ = true;
    }

private:
    // The readout copies three 8x8 windows of the status layer for every
    // life icon: the icon's own record and the two that follow it.
    void capture_lives_windows(const simulation::SnesPpuState& ppu) noexcept {
        lives_window_count_ = 0U;
        lives_windows_unbounded_ = false;
        for (unsigned object = 0U; object + 2U < 128U; ++object) {
            const auto base = object * 4U;
            const auto tile = unsigned(ppu.oam[base + 2U]) | ((ppu.oam[base + 3U] & 1U) << 8U);
            if (tile != 189U) continue;
            for (unsigned part = 0U; part < 3U; ++part) {
                const auto record = (object + part) * 4U;
                const int x = ppu.oam[record], y = ppu.oam[record + 1U];
                // A window past the layer's edge reads its neighbouring rows.
                if (x + 8 > 256 || y + 8 > 224
                    || lives_window_count_ == lives_windows_.size()) {
                    lives_windows_unbounded_ = true;
                    return;
                }
                lives_windows_[lives_window_count_++] = {
                    static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)};
            }
        }
    }
    [[nodiscard]] bool meters_reach_lives(const simulation::MeterState& meter) const noexcept {
        if (lives_window_count_ == 0U) return false;
        const auto rectangles = render::meter_rectangles(meter, 256U);
        for (std::size_t i = 0U; i < rectangles.count; ++i) {
            const auto& r = rectangles.rectangles[i];
            for (std::size_t n = 0U; n < lives_window_count_; ++n) {
                const int x = lives_windows_[n][0], y = lives_windows_[n][1];
                if (r.x < x + 8 && x < r.x + r.width && r.y < y + 8 && y < r.y + r.height)
                    return true;
            }
        }
        return false;
    }

    SpriteSourceCache sprites_{SpriteSourceCache::Selection::bottom_hud};
    Sources sources_{};
    // The whole picture's sources at the last capture.
    simulation::MeterState meter_{};
    simulation::DialogueState dialogue_{};
    simulation::MeterState lives_meter_{};
    // The last meters known to stay clear of the readout's windows.
    simulation::MeterState lives_clear_meter_{};
    std::array<std::array<std::uint8_t, 2U>, 12U> lives_windows_{};
    std::size_t lives_window_count_{};
    bool lives_windows_unbounded_{};
    bool lives_reads_meters_{};
    render::Palette256 palette_{};
    std::array<Colours, element_count> used_{};
    bool valid_{};
};
}
