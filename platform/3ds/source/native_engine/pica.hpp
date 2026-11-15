#pragma once
#include "scene.hpp"
#include "starfox/render/palette.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <array>

namespace starfox::platform_3ds::native_engine {
struct GpuStats { std::uint32_t build_us{}, flush_us{}, vertices{}, draws{}; };
class Pica {
public:
    ~Pica() { close(); }
    bool open();
    void close() noexcept;
    void abandon() noexcept { abandoned_ = true; }
    bool ready() const noexcept { return !abandoned_ && target_ != nullptr; }
    // One Citro2D batch, with its ordinary shader and both of the texture
    // coordinates that shader always supplies. Texture unit 2 is never used.
    bool draw(const Scene&, const render::Palette256&);
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
