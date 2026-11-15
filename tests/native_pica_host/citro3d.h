#pragma once
#include <3ds.h>
#include <cstdlib>
#include <set>
#include <cassert>
struct C3D_Tex {void* data{};unsigned width{},height{},size{};};
struct C3D_RenderTarget{};
enum {GPU_RGBA8,GPU_NEAREST,GPU_REPEAT,GPU_TEXFACE_2D,GPU_CLAMP_TO_EDGE,
    GX_CMDLIST_FLUSH,C3D_CLEAR_COLOR,GPU_SCISSOR_DISABLE,GPU_ALWAYS,GPU_WRITE_COLOR,
    GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_GEQUAL,GPU_WRITE_ALL};
namespace pica_test { inline std::set<void*> owners;inline unsigned scenes=0,binds=0; }
inline bool C3D_TexInit(C3D_Tex* t,unsigned w,unsigned h,int){
    t->data=std::calloc(w*h,4);t->width=w;t->height=h;t->size=w*h*4;
    if(t->data)pica_test::owners.insert(t->data);return t->data!=nullptr;
}
inline bool C3D_TexInitVRAM(C3D_Tex* t,unsigned w,unsigned h,int f){return C3D_TexInit(t,w,h,f);}
inline void C3D_TexDelete(C3D_Tex* t){assert(pica_test::owners.erase(t->data)==1);std::free(t->data);}
inline void C3D_TexSetFilter(C3D_Tex*,int,int){}
inline void C3D_TexSetWrap(C3D_Tex*,int,int){}
inline C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex*,int,int,int){return new C3D_RenderTarget;}
inline void C3D_RenderTargetDelete(C3D_RenderTarget* t){delete t;}
inline void C3D_RenderTargetClear(C3D_RenderTarget*,int,int,int){}
inline void C3D_FrameSplit(int){}
inline void C3D_SetScissor(int,unsigned,unsigned,unsigned,unsigned){}
inline void C3D_DepthTest(bool,int,int){}
inline void C3D_AlphaTest(bool,int,int){}
inline void C3D_AlphaBlend(int,int,int,int,int,int){}
inline void C3D_TexBind(unsigned unit,C3D_Tex*){assert(unit<2);++pica_test::binds;}
