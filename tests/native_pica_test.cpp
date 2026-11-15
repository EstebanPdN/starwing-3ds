#include "native_engine/pica.hpp"
#include "native_engine/material.hpp"
#include <citro2d.h>
#include <memory>
#include <cstdio>
using namespace starfox;
using namespace platform_3ds::native_engine;
int main(){
    auto pica=std::make_unique<Pica>();assert(pica->open());
    render::Palette256 palette{};
    for(unsigned i=0;i<256;++i)palette[i]={std::uint8_t(i),std::uint8_t(i^85),std::uint8_t(i^170),255};
    render::RasterCommands cmds;cmds.reset(400,240);
    render::RasterCommand r{};r.left=1;r.top=2;r.right=11;r.bottom=12;r.even=1;r.odd=2;r.dither=true;
    cmds.commands.push_back(r);Scene scene;assert(scene.compile(cmds));
    assert(pica->draw(scene,palette));assert(pica_test::quads.size()==1);
    assert(pica->stats().vertices==4 && pica->stats().draws==1);
    assert(pica_test::quads[0].tile[palette_texel_offset(1)]==material_colour(palette,1));
    const auto clean=pica_test::cleans;
    pica_test::begin();assert(pica->draw(scene,palette));assert(pica_test::cleans==clean);
    palette[1].r^=255;pica_test::begin();assert(pica->draw(scene,palette));
    assert(pica_test::cleans>clean && pica_test::quads[0].tile[palette_texel_offset(1)]==material_colour(palette,1));
    // A rejected frame must not leave supposedly valid, unflushed tiles.
    palette[1].b^=255;pica_test::cache_fail=true;pica_test::begin();
    assert(!pica->draw(scene,palette) && pica_test::quads.empty());
    pica_test::cache_fail=false;pica_test::begin();assert(pica->draw(scene,palette));
    assert(pica_test::quads[0].tile[palette_texel_offset(1)]==material_colour(palette,1));
    pica_test::begin();pica_test::available=8;
    const auto scenes=pica_test::scenes;
    assert(!pica->draw(scene,palette) && pica_test::scenes==scenes);
    // 1025 distinct pairs remain one batch, with no material limit or
    // per-quad texture binding. Order and palette-zero alpha are retained.
    cmds.commands.clear();
    for(unsigned i=0;i<1025;++i){
        r.left=i%400;r.right=r.left+1;r.top=i/400;r.bottom=r.top+1;
        r.even=i&255;r.odd=i>>8;cmds.commands.push_back(r);
    }
    assert(scene.compile(cmds));pica_test::begin();
    const auto binds=pica_test::binds;
    assert(pica->draw(scene,palette));
    assert(pica_test::quads.size()==1025 && pica->stats().draws==1);
    assert(pica_test::binds-binds==2);
    for(unsigned i=0;i<1025;++i){
        const auto& q=pica_test::quads[i];
        assert(q.points[0].colour==material_vertex_colour(palette,i&255));
        assert(q.tile[palette_texel_offset(i>>8)]==material_colour(palette,i>>8));
    }
    pica.reset();assert(pica_test::owners.empty());
    std::puts("PASS: native GPU command construction, palette reuse/update, flush failure, limits and resource ownership");
}
