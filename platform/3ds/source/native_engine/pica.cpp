#include "pica.hpp"
#include "material.hpp"
#include "../cache_3ds.h"
#include <citro2d.h>
#include <algorithm>
#include <cstring>

namespace starfox::platform_3ds::native_engine {
namespace {
std::uint32_t elapsed(std::uint64_t tick) {
    return std::uint32_t((svcGetSystemTick()-tick)*1'000'000ULL/SYSCLOCK_ARM11);
}
}
bool Pica::open() {
    if (ready()) return true;
    if (!C3D_TexInit(&palette_,16,16,GPU_RGBA8)) return false;
    if (!C3D_TexInit(&checker_,8,8,GPU_RGBA8)) { close(); return false; }
    C3D_TexSetFilter(&palette_,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&palette_,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&checker_,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&checker_,GPU_REPEAT,GPU_REPEAT);
    write_material(static_cast<u32*>(checker_.data),0,0xffffffffU);
    if (R_FAILED(starwing_clean_device_cache(checker_.data,256))) { close(); return false; }
    if (!C3D_TexInitVRAM(&world_,512,256,GPU_RGBA8)) { close(); return false; }
    target_=C3D_RenderTargetCreateFromTex(&world_,GPU_TEXFACE_2D,0,-1);
    if (!target_) { close(); return false; }
    C3D_TexSetFilter(&world_,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&world_,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    return true;
}
void Pica::close() noexcept {
    if (abandoned_) return;
    if (target_) C3D_RenderTargetDelete(target_);
    target_=nullptr;
    if (world_.data) C3D_TexDelete(&world_);
    if (checker_.data) C3D_TexDelete(&checker_);
    if (palette_.data) C3D_TexDelete(&palette_);
    world_={}; checker_={}; palette_={};palette_valid_=false;
}
bool Pica::prepare_palette(const render::Palette256& palette) {
    // The texture words and vertex colours are functions of the palette
    // alone: the palette they were last made from needs no conversion.
    if (palette_valid_ && std::memcmp(palette_source_.data(),palette.data(),
            sizeof(palette_source_))==0) return true;
    std::array<std::uint32_t,256> words;
    for(unsigned i=0;i<256;++i) words[palette_texel_offset(i)]=material_colour(palette,i);
    if (palette_valid_ && words==palette_words_) { palette_source_=palette; return true; }
    palette_words_=words;palette_valid_=false;
    for(unsigned i=0;i<256;++i)vertex_colours_[i]=material_vertex_colour(palette,i);
    std::memcpy(palette_.data,words.data(),sizeof(words));
    const auto begin=svcGetSystemTick();
    const bool ok=R_SUCCEEDED(starwing_clean_device_cache(palette_.data,sizeof(words)));
    stats_.flush_us=elapsed(begin);palette_valid_=ok;
    if (ok) palette_source_=palette;
    return ok;
}
bool Pica::draw(const Scene& scene,const render::Palette256& palette) {
    if (!ready() || scene.stats().overflow) return false;
    stats_={};const auto begin=svcGetSystemTick();
    if (scene.rectangles().size()+8U>C2D_QuadsAvailable() || !prepare_palette(palette)) return false;
    C2D_Flush();C3D_FrameSplit(GX_CMDLIST_FLUSH);
    C3D_RenderTargetClear(target_,C3D_CLEAR_COLOR,0,0);
    C2D_SceneBegin(target_);C2D_ViewReset();
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,512,256);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    // Replace, not blend: a later rectangle overwrites colour and alpha, so
    // palette zero (alpha 0) erases geometry drawn before it.
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    // Both texcoord0 and texcoord1 are outputs of the standard Citro2D
    // shader, in every mode. Unit 1 keeps this valid texture bound through
    // the final 2D pass; no private Citro3D context mutation/unbind is needed.
    C3D_TexBind(0,&palette_);C3D_TexBind(1,&checker_);
    auto* vertices=scene.rectangles().empty()?nullptr:
        C2D_ReserveDitherQuads(&palette_,scene.rectangles().size());
    if (vertices) {
        static constexpr auto palette_coordinates=material_palette_coordinates();
        for(const auto& r:scene.rectangles()) {
            write_material_quad(r,vertex_colours_[r.even],palette_coordinates,vertices);
            vertices+=4;
        }
        stats_.vertices=std::uint32_t(scene.rectangles().size()*4U);
    }
    C2D_Flush();stats_.draws=stats_.vertices?1:0;
    // Restore alpha blending and Citro2D's depth test for the passes that follow.
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
    stats_.build_us=elapsed(begin)-stats_.flush_us;
    return stats_.vertices==scene.rectangles().size()*4U;
}
} // namespace starfox::platform_3ds::native_engine
