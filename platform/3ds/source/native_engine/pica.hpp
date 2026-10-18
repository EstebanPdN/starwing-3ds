#pragma once
#include "scene.hpp"
#include "starfox/render/palette.hpp"
#include <3ds.h>
#include <citro3d.h>

namespace starfox::platform_3ds::native_engine {
struct GpuStats { std::uint32_t build_us{}, flush_us{}, vertices{}, draws{}; };
class Pica {
public:
    ~Pica() { close(); }
    bool open();
    void close() noexcept;
    bool ready() const noexcept { return vertices_ != nullptr; }
    // The caller fences vertex reuse and owns FrameBegin/FrameEnd.
    bool draw(const Scene&, const render::Palette256&);
    C3D_Tex* texture() noexcept { return &world_; }
    const GpuStats& stats() const noexcept { return stats_; }
private:
    struct Vertex { std::int16_t x,y; std::uint8_t even,odd; std::int16_t scale; };
    static_assert(sizeof(Vertex)==8);
    Vertex* vertices_{};
    DVLB_s* shader_{};
    shaderProgram_s program_{};
    C3D_Tex palette_{}, checker_{}, world_{};
    C3D_RenderTarget* target_{};
    C3D_Mtx projection_{};
    int projection_uniform_{};
    GpuStats stats_{};
};
} // namespace starfox::platform_3ds::native_engine
