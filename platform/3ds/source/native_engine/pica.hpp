#pragma once
#include "scene.hpp"
#include "starfox/render/palette.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <array>
// PICA200 drawing of a native_engine::Scene into a 512x256 RGBA8 world texture
// in VRAM. Owned by Display3ds; tests/native_pica_test.cpp runs it on the host
// against the stubs in tests/native_pica_host.

namespace starfox::platform_3ds::native_engine {
// The last draw(): CPU microseconds spent building the batch and cleaning the
// palette texture from the data cache, vertices written and batches drawn.
struct GpuStats { std::uint32_t build_us{}, flush_us{}, vertices{}, draws{}; };
class Pica {
public:
    ~Pica() { close(); }
    // Creates the palette and checker textures and the world render target.
    // On failure releases what it made and returns false; true if already open.
    bool open();
    // Releases the textures, except after abandon(): a lost GPU may still use them.
    void close() noexcept;
    // Called once the GPU is lost: ready() stays false from then on.
    void abandon() noexcept { abandoned_ = true; }
    bool ready() const noexcept { return !abandoned_ && target_ != nullptr; }
    // One Citro2D batch, with its ordinary shader and both of the texture
    // coordinates that shader always supplies. Texture unit 2 is never used.
    // Clears the world texture and draws every rectangle of the scene. False
    // when not ready, the scene overflowed, Citro2D lacks room for the quads
    // or the palette upload fails.
    bool draw(const Scene&, const render::Palette256&);
    // The world texture draw() renders into.
    C3D_Tex* texture() noexcept { return &world_; }
    const GpuStats& stats() const noexcept { return stats_; }
private:
    bool prepare_palette(const render::Palette256&);
    // The palette palette_words_ and vertex_colours_ were made from; only
    // meaningful while palette_valid_.
    render::Palette256 palette_source_{};
    std::array<std::uint32_t,256> palette_words_{};
    std::array<std::uint32_t,256> vertex_colours_{};
    C3D_Tex palette_{},checker_{},world_{};
    C3D_RenderTarget* target_{};
    GpuStats stats_{};
    bool palette_valid_{},abandoned_{};
};
} // namespace starfox::platform_3ds::native_engine
