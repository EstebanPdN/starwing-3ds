#include "starfox/assets/shape_decoder.hpp"
#include "starfox/render/face_material.hpp"
#include "starfox/render/software_renderer.hpp"
#include "starfox/render/source_sprite_material.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#if defined(STARFOX_TEST_SOURCE_SCENE)
#include "starfox/vr/source_models.hpp"
#endif

int main(int argc,char** argv) try {
    if(argc!=3) return 2;
    const auto rom=starfox::assets::RomImage::load(argv[1]);
    const auto symbols=starfox::assets::SymbolMap::load(argv[2]);
    const starfox::assets::ShapeDecoder decoder(rom,symbols);
    const auto address=symbols.find("FIREBALL").at(0);
    const auto table=std::uint16_t(symbols.find("BONFIRE_C").at(0));
    std::unordered_map<std::uint64_t,starfox::assets::Shape> cache;
    for(unsigned colour:{0U,1U,0U,1U}) {
        starfox::simulation::GameObject object;
        object.strategy_flags[0]=0x20U;object.extended[21]=colour;
        const auto key=starfox::render::source_shape_material_key(address,table,object);
        auto found=cache.find(key);
        if(found==cache.end()) found=cache.emplace(key,decoder.decode(address,{},table,
            starfox::render::source_sprite_colour_count(object))).first;
        const auto& shape=found->second;
        if(!starfox::render::texture_for_colour(shape,colour,0))
            throw std::runtime_error("Dancing Insector fire trail material missing: "+std::to_string(colour));
        const auto lod=decoder.decode_lod(shape.header,address&0xffffU,table,
            starfox::render::source_sprite_colour_count(object));
        if(!starfox::render::texture_for_colour(lod,colour,1))
            throw std::runtime_error("LOD dropped runtime-selected sprite material");
        starfox::render::RenderPose pose;pose.simple_scaled_sprite=true;
        pose.simple_sprite_colour=colour;pose.simple_sprite_world_size=120;
        pose.z=500;
        starfox::render::Framebuffer frame(224,192);
        starfox::render::SoftwareRenderer{}.draw(shape,pose,frame);
        if(std::none_of(frame.pixels().begin(),frame.pixels().end(),[](auto p){return p!=0;}))
            throw std::runtime_error("Fire lead/trail sprite rendered no pixels");
    }
    if(cache.size()!=2U) throw std::runtime_error("Lead and trail sprite cache variants collided");
#if defined(STARFOX_TEST_SOURCE_SCENE)
    // The same source assembler feeds VR, Leia and MODERN. Exercise its own
    // cache, not just the desktop key helper/decoder in isolation.
    starfox::vr::GameSceneSnapshot scene;
    scene.flow=starfox::simulation::GameFlowState::gameplay;
    scene.model_palette.fill(0x7fffU);
    for(unsigned colour:{0U,1U}) {
        starfox::vr::GameSceneObject item;item.handle=colour+1U;
        item.object.shape=address&0xffffU;item.object.colour_table=table;
        item.object.strategy_flags[0]=0x20U;item.object.extended[21]=colour;
        item.source_pose.simple_scaled_sprite=true;
        item.source_pose.z=item.source_pose.source_depth=500;
        scene.objects.push_back(item);
    }
    starfox::vr::SourceModels models(rom,symbols);
    for(unsigned pass=0;pass<2;++pass) {
        const auto packets=models.assemble(scene);
        if(!packets.pending.empty()) throw std::runtime_error(packets.pending.front().reason);
        if(packets.packets.size()!=2U) throw std::runtime_error("Shared source scene lost a fire sprite");
        for(const auto& packet:packets.packets)
            if(packet.geometry.vertex_view().empty() || packet.geometry.texel_view().empty())
                throw std::runtime_error("Shared source scene emitted empty fire geometry/texture");
        std::reverse(scene.objects.begin(),scene.objects.end());
    }
#endif
    bool rejected=false;
    try { (void)decoder.decode(address,{},table,257U); }
    catch(const std::invalid_argument&) { rejected=true; }
    if(!rejected) throw std::runtime_error("Out-of-range material count accepted");
    std::cout<<"Source fire lead and trail materials both render\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
