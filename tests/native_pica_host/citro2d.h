#pragma once
#include <citro3d.h>
#include <array>
#include <vector>
#include <cstring>
#include <algorithm>
struct C2D_DitherVertex {float x,y,u,v,checker_u,checker_v;u32 colour;};
struct C2D_Vertex {float pos[3],texcoord[2],ptcoord[2];u32 color;};
namespace pica_test {
inline std::size_t available=16384;
struct Quad{std::array<u32,256> tile;std::array<C2D_DitherVertex,4> points;};
inline std::vector<Quad> quads;
inline std::vector<C2D_Vertex> vertices;
inline C3D_Tex* pending_palette{};
inline void begin(){quads.clear();vertices.clear();pending_palette=nullptr;available=16384;}
}
inline std::size_t C2D_QuadsAvailable(){return pica_test::available;}
inline bool C2D_DrawDitherQuad(C3D_Tex* texture,const C2D_DitherVertex* v){
    if(!pica_test::available)return false;
    --pica_test::available;
    assert(texture->width==16 && texture->height==16 && texture->size==1024);
    pica_test::Quad q;std::memcpy(q.tile.data(),texture->data,1024);
    std::copy_n(v,4,q.points.data());pica_test::quads.push_back(q);return true;
}
inline C2D_Vertex* C2D_ReserveDitherQuads(C3D_Tex* texture,std::size_t count) {
    if(!texture || !count || count>pica_test::available)return nullptr;
    assert(pica_test::vertices.empty());
    pica_test::available-=count;pica_test::vertices.resize(count*4);
    pica_test::pending_palette=texture;return pica_test::vertices.data();
}
inline void C2D_Flush(){
    for(std::size_t n=0;n<pica_test::vertices.size();n+=4) {
        pica_test::Quad q;std::memcpy(q.tile.data(),pica_test::pending_palette->data,1024);
        for(unsigned i=0;i<4;++i){const auto&v=pica_test::vertices[n+i];
            q.points[i]={v.pos[0],v.pos[1],v.texcoord[0],v.texcoord[1],v.ptcoord[0],v.ptcoord[1],v.color};}
        pica_test::quads.push_back(q);
    }
    pica_test::vertices.clear();pica_test::pending_palette=nullptr;
}
inline void C2D_ViewReset(){}
inline void C2D_SceneBegin(C3D_RenderTarget*){++pica_test::scenes;}
