#pragma once
#include "starfox/simulation/snes_ppu.hpp"
#include "starfox/vr/draw_packet.hpp"
#include <array>
#include <optional>
#include <vector>

namespace starfox::vr {
enum class BackgroundLayer { bg1, bg2, bg3 };
// Opaque enclosure with a 256x224 source-window opening. Draw after world
// geometry with depth disabled, before HUD; caller supplies source placement.
[[nodiscard]] DrawPacket tunnel_surround_packet(const std::array<float,4>& colour);
// Opaque CGRAM-zero backdrop, with source integer fading followed by target
// color-space conversion. Shared by both eyes, independent of layer enables.
[[nodiscard]] std::array<float,4> source_backdrop_colour(uint16_t bgr555,
    unsigned brightness,bool srgb=false);
[[nodiscard]] std::array<float,4> source_background_border_colour(
    const std::array<uint16_t,256>& palette,unsigned brightness,bool srgb=false);
[[nodiscard]] std::array<float,4> source_menu_background_colour(
    const simulation::SnesPpuState&,unsigned brightness,bool srgb=false);
struct BackgroundTileOptions {
    unsigned priority{}; // 0 all, 1 low, 2 high
    unsigned brightness{15}; // source display brightness, 0..15
    bool expanded_horizontal{};
    bool transparent_black{}; // Test source palette before brightness fading.
    bool wrap_horizontal{true}; // False: expose only one authored tilemap occurrence.
    unsigned single_occurrence_top_rows{}; // 0..224: unique top band; 512: one complete atlas in both axes.
    bool ex_twin_planets{}; // BG_5_4: unique planet ink, repeatable cloud tiles.
    bool ex_face_planets{}; // BG_6_3H: unique planets/saucers, repeatable stars.
    bool ex_ocean_island{}; // BG_6_2: preserve the first island, repeat open water.
    bool ex_volcanic_horizon{}; // BG_6_6: one bright eruption on a repeatable horizon.
    bool ex_city_planets{}; // BG_5_2: unique planets/palace, surrounding stars/city.
    unsigned guard_inset{}; // Native pixel columns omitted from each side.
    std::array<int32_t,2> horizontal_bounds{0,256}; // Logical pixels, not stretched UVs.
    std::optional<std::array<int16_t,2>> scroll_override;
};
// Encode source data only: no CPU pixel decoding, projection, or layer order.
// Caller supplies brightness and owns screen enables, clipping and composition.
// Unsupported source modes throw instead of silently substituting a layer.
[[nodiscard]] std::vector<uint32_t> background_tile_payload(
    const simulation::SnesPpuState&,BackgroundLayer,
    const BackgroundTileOptions& = {});
// Logical-coordinate quad (256x224 by default); caller supplies its world transform.
// Disabled main-screen layers produce empty packets, without uploading VRAM.
[[nodiscard]] DrawPacket background_tile_packet(const simulation::SnesPpuState&,
    BackgroundLayer,const BackgroundTileOptions& = {},bool srgb=false);
// Original intro DEMO atlas: its upper 256 rows contain stars only.
[[nodiscard]] DrawPacket intro_star_sphere_packet(const simulation::SnesPpuState&,
    unsigned brightness=15,bool srgb=false,bool full_atlas=false,bool upper_left_only=false,
    bool retain_source_scroll=false);
[[nodiscard]] DrawPacket intro_planet_packet(const simulation::SnesPpuState&,
    unsigned brightness=15,bool srgb=false);
[[nodiscard]] DrawPacket unique_planet_packet(const simulation::SnesPpuState&,
    const BackgroundTileOptions&,const std::array<unsigned,4>& rectangle,bool srgb=false);
[[nodiscard]] Matrix4 intro_planet_motion(int16_t previous_scroll,int16_t current_scroll,
    double alpha,float horizon_y=112);
[[nodiscard]] Matrix4 landscape_pitch_motion(uint16_t previous_pitch,uint16_t current_pitch,double alpha);
[[nodiscard]] Matrix4 landscape_scroll_motion(int16_t previous_scroll,int16_t current_scroll,double alpha,
    uint16_t previous_bank=0,uint16_t current_bank=0,uint16_t atlas_origin=232);
[[nodiscard]] DrawPacket landscape_sphere_packet(const simulation::SnesPpuState&,
    const BackgroundTileOptions&,float horizon_y=112,bool srgb=false,bool unique_half=false,uint16_t atlas_origin=232,bool unique_right_half=false);
// Lower hemisphere becomes a receiver at the grid's camera-relative Y.
// Upper sky/mountains retain the enclosing background projection.
void place_landscape_ground(DrawPacket&,float ground_y,bool gpu=false);
[[nodiscard]] DrawPacket water_surround_packet(const simulation::SnesPpuState&,
    const BackgroundTileOptions&,bool srgb=false);
// Inverse-projected Mode-1 foreground. Height is camera-to-receiver distance.
[[nodiscard]] DrawPacket water_surface_packet(const simulation::SnesPpuState&,
    const BackgroundTileOptions&,float height,bool srgb=false);
[[nodiscard]] Matrix4 water_height_motion(float previous_height,float current_height,double alpha);
// Orbital horizon: preserve the authored vertical band instead of clamping
// its final texture row over a hemisphere. Unique upper rows remain unique.
[[nodiscard]] DrawPacket space_horizon_sphere_packet(const simulation::SnesPpuState&,
    const BackgroundTileOptions&,bool srgb=false);
// Original LSB atlas: stars above the horizon, planet across the entire lower hemisphere.
[[nodiscard]] Matrix4 orbital_horizon_motion(float previous_scroll,float current_scroll,double alpha,
    bool thin_horizon=false,bool entry_horizon=false,bool gameplay=false);
[[nodiscard]] DrawPacket orbital_planet_sphere_packet(const simulation::SnesPpuState&,
    const BackgroundTileOptions&,bool srgb=false,bool thin_horizon=false,bool entry_horizon=false,bool gameplay=false);
// Source Mode-1 title overlays, in painter order after models and before OAM.
// BG1 is omitted for EX's introductory logo backdrop. Black palette entries
// are opaque here: they supply outlines and intentional model occlusion.
[[nodiscard]] std::vector<DrawPacket> title_foreground_packets(
    const simulation::SnesPpuState&,unsigned brightness,bool include_bg1,
    std::optional<std::array<int16_t,2>> bg2_scroll={},bool srgb=false);
}
