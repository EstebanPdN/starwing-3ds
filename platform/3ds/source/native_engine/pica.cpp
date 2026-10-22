#include "pica.hpp"
#include <citro2d.h>
#include <span_shbin.h>
#include <algorithm>

namespace starfox::platform_3ds::native_engine {
namespace {
unsigned morton(unsigned x,unsigned y,unsigned width) {
    unsigned offset=((y>>3)*(width>>3)+(x>>3))*64;
    for (unsigned bit=0;bit<3;++bit) {
        offset|=((x>>bit)&1U)<<(bit*2);
        offset|=((y>>bit)&1U)<<(bit*2+1);
    }
    return offset;
}
std::uint32_t elapsed(std::uint64_t tick) {
    return std::uint32_t((svcGetSystemTick()-tick)*1'000'000ULL/SYSCLOCK_ARM11);
}
}
bool Pica::open() {
    if (ready()) return true;
    shader_=DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(span_shbin)),span_shbin_size);
    if (!shader_) return false;
    shaderProgramInit(&program_);
    shaderProgramSetVsh(&program_,&shader_->DVLE[0]);
    projection_uniform_=shaderInstanceGetUniformLocation(program_.vertexShader,"projection");
    if (!C3D_TexInit(&palette_,16,16,GPU_RGBA8)
        || !C3D_TexInit(&checker_,8,8,GPU_RGBA8)) { close(); return false; }
    if (!C3D_TexInitVRAM(&world_,512,256,GPU_RGBA8)) { close(); return false; }
    target_=C3D_RenderTargetCreateFromTex(&world_,GPU_TEXFACE_2D,0,-1);
    if (!target_) { close(); return false; }
    C3D_TexSetFilter(&world_,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&world_,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    vertices_=static_cast<Vertex*>(linearAlloc(Scene::capacity*6*sizeof(Vertex)));
    if (!vertices_) { close(); return false; }
    C3D_TexSetFilter(&palette_,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&palette_,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&checker_,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&checker_,GPU_REPEAT,GPU_REPEAT);
    auto* pixels=static_cast<u32*>(checker_.data);
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x)
        pixels[morton(x,y,8)]=((x^y)&1U)?0xffffffffU:0x00000000U;
    C3D_TexFlush(&checker_);
    Mtx_Ortho(&projection_,0,512,256,0,-1,1,true);
    return true;
}
void Pica::close() noexcept {
    if (abandoned_) return;
    if (target_) C3D_RenderTargetDelete(target_);
    target_=nullptr;
    if (world_.data) C3D_TexDelete(&world_);
    world_={};
    if (vertices_) linearFree(vertices_);
    vertices_=nullptr;
    if (palette_.data) C3D_TexDelete(&palette_);
    if (checker_.data) C3D_TexDelete(&checker_);
    palette_={};checker_={};
    if (shader_) { shaderProgramFree(&program_); DVLB_Free(shader_); }
    shader_=nullptr;
}
bool Pica::draw(const Scene& scene,const render::Palette256& colours) {
    if (!ready() || scene.stats().overflow) return false;
    stats_={}; auto begin=svcGetSystemTick();
    auto* palette=static_cast<u32*>(palette_.data);
    for (unsigned i=0;i<256;++i) {
        const auto c=colours[i];
        palette[morton(i&15U,i>>4,16)]=(u32(c.r)<<24)|(u32(c.g)<<16)|(u32(c.b)<<8)|(i?255U:0U);
    }
    auto* out=vertices_;
    for (const auto& r:scene.rectangles()) {
        const auto put=[&](std::int16_t x,std::int16_t y) {
            *out++={x,y,r.even,r.odd,std::int16_t(r.scale)};
        };
        put(r.left,r.top);put(r.right,r.top);put(r.right,r.bottom);
        put(r.left,r.top);put(r.right,r.bottom);put(r.left,r.bottom);
    }
    stats_.vertices=std::uint32_t(out-vertices_);
    stats_.build_us=elapsed(begin); begin=svcGetSystemTick();
    C3D_TexFlush(&palette_);
    if (stats_.vertices && R_FAILED(GSPGPU_FlushDataCache(vertices_,stats_.vertices*sizeof(Vertex)))) return false;
    stats_.flush_us=elapsed(begin);
    // Queue the pending Citro2D draws, and flush their command list, before
    // the world target's fill so every GPU operation keeps program order.
    C2D_Flush();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    C3D_RenderTargetClear(target_,C3D_CLEAR_COLOR,0,0);
    C3D_FrameDrawOn(target_);
    C3D_BindProgram(&program_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_uniform_,&projection_);
    C3D_AttrInfo attributes;AttrInfo_Init(&attributes);
    AttrInfo_AddLoader(&attributes,0,GPU_SHORT,2);
    AttrInfo_AddLoader(&attributes,1,GPU_UNSIGNED_BYTE,2);
    AttrInfo_AddLoader(&attributes,2,GPU_SHORT,1);
    C3D_SetAttrInfo(&attributes);
    C3D_BufInfo buffers;BufInfo_Init(&buffers);
    BufInfo_Add(&buffers,vertices_,sizeof(Vertex),3,0x210);
    C3D_SetBufInfo(&buffers);
    C3D_TexBind(0,&palette_);C3D_TexBind(1,&palette_);C3D_TexBind(2,&checker_);
    for (int i=0;i<6;++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    auto* env=C3D_GetTexEnv(0);
    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE0,GPU_TEXTURE0);
    C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    env=C3D_GetTexEnv(1);
    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE1,GPU_PREVIOUS,GPU_TEXTURE2);
    C3D_TexEnvFunc(env,C3D_Both,GPU_INTERPOLATE);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    if (stats_.vertices) { C3D_DrawArrays(GPU_TRIANGLES,0,stats_.vertices); stats_.draws=1; }
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
    // Citro3D 1.7.1 dereferences tex for units 1/2, even when unbinding.
    // Keep these owned textures bound: C2D_Prepare restores the combiners,
    // and Citro2D samples only unit 0. The next native pass rebinds all units.
    C2D_Prepare();
    return true;
}
} // namespace starfox::platform_3ds::native_engine
