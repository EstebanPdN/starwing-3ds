#include "starfox/vr/vulkan_draw_packets.hpp"
#include "starfox/vr/source_span_layout.hpp"
#include "starfox/vr/source_models.hpp"
#include "starfox/vr/vulkan_scene_buffer.hpp"
#include "starfox/vr/vulkan_scene_textures.hpp"
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cmath>
#include <bit>

namespace starfox::vr {
struct VulkanDrawPackets::State {
    struct Geometry {DrawPacket source;std::shared_ptr<VulkanSceneBuffer> triangles,lines;std::shared_ptr<VulkanSceneTextures> textures;bool ray_positions{};};
    struct Item {Matrix4 model;std::shared_ptr<Geometry> geometry;uint32_t key{};};
    struct Pipelines {VulkanSceneTextures layout;VulkanScenePipeline triangles,lines;bool triangles_ready{},lines_ready{};};
    VkDevice device{};PFN_vkGetDeviceProcAddr get{};VkRenderPass pass{};
    std::vector<Item> items;
    std::unordered_map<uint32_t,size_t> ray_items;
    std::shared_ptr<Pipelines> pipelines;
    std::size_t reused{},uploaded{};
    std::size_t vertex_uploads{},texture_uploads{};
    bool keyed{};
    bool depth_test{true};
};
VulkanDrawPackets::VulkanDrawPackets()=default;
VulkanDrawPackets::~VulkanDrawPackets()=default;
void VulkanDrawPackets::close() noexcept {state_.reset();}
std::size_t VulkanDrawPackets::size() const noexcept {return state_?state_->items.size():0;}
std::size_t VulkanDrawPackets::reused_packets() const noexcept {return state_?state_->reused:0;}
std::size_t VulkanDrawPackets::uploaded_packets() const noexcept {return state_?state_->uploaded:0;}
std::size_t VulkanDrawPackets::uploaded_vertex_buffers() const noexcept {return state_?state_->vertex_uploads:0;}
std::size_t VulkanDrawPackets::uploaded_texture_buffers() const noexcept {return state_?state_->texture_uploads:0;}
bool VulkanDrawPackets::ray_source(uint32_t key,RaySource& output) const {
    if(!state_ || !state_->keyed) return false;
    const auto found=state_->ray_items.find(key);
    if(found!=state_->ray_items.end()) {
        const auto& item=state_->items[found->second];
        const auto& geometry=*item.geometry;
        if(!geometry.triangles || !geometry.triangles->count() || !geometry.ray_positions) return false;
        output={{geometry.triangles->buffer(),0,uint64_t(geometry.triangles->count())*sizeof(SceneVertex)},
            geometry.triangles->count(),item.model};return true;
    }
    return false;
}
bool VulkanDrawPackets::update_models(std::span<const Matrix4> models) noexcept {
    if(!state_ || models.size()!=state_->items.size()) return false;
    for(const auto& model:models) if(!model_eye_camera(EyeCamera{},model)) return false;
    for(std::size_t i=0;i<models.size();++i) state_->items[i].model=models[i];
    return true;
}
bool VulkanDrawPackets::initialize(VkDevice device,PFN_vkGetDeviceProcAddr get,
    const VkPhysicalDeviceMemoryProperties& memory,VkRenderPass pass,std::span<const DrawPacket> packets,std::span<const uint32_t> object_keys,bool depth_test) {
    try {
        if(!device || !get || !pass || packets.size()>4096) throw std::runtime_error("Invalid native scene upload");
        if(!object_keys.empty()) {
            if(object_keys.size()!=packets.size()) throw std::runtime_error("Native scene key count mismatch");
            std::unordered_set<uint32_t> unique;
            for(auto key:object_keys) if(!unique.insert(key).second) throw std::runtime_error("Duplicate native scene object key");
        }
        std::size_t vertex_count=0,texel_count=0;
        // Validate the entire submission before allocating GPU resources.
        for(const auto& packet:packets) {
            const auto& mesh=packet.geometry;
            const auto vertices=mesh.vertex_view();
            if((mesh.shared_vertices && !mesh.vertices.empty()) || (mesh.shared_line_vertices && !mesh.line_vertices.empty()) || !mesh.deferred.empty() || vertices.size()%3 || mesh.line_view().size()%2
                || !model_eye_camera(EyeCamera{},packet.model))
                throw std::runtime_error("Invalid native scene packet: triangles="+std::to_string(vertices.size())
                    +" lines="+std::to_string(mesh.line_view().size())+" deferred="+std::to_string(mesh.deferred.size())
                    +" shared="+std::to_string(bool(mesh.shared_vertices))+" owned="+std::to_string(mesh.vertices.size())
                    +" transform="+std::to_string(bool(model_eye_camera(EyeCamera{},packet.model))));
            if(vertices.size()>4'000'000 || mesh.line_view().size()>4'000'000 || mesh.texels.size()>4'000'000)
                throw std::runtime_error("Native scene packet exceeds upload budget");
            vertex_count+=vertices.size()+mesh.line_view().size();texel_count+=mesh.texels.size();
            if(vertex_count>4'000'000 || texel_count>4'000'000) throw std::runtime_error("Native scene exceeds upload budget");
            std::optional<std::size_t> checked_grid_words;
            std::optional<std::size_t> checked_dust_words;
            bool checked_connected_rows=false;
            std::unordered_set<uint64_t> checked_span_headers;
            for(const auto list:{vertices,mesh.line_view()}) for(const auto& v:list)
                if((v.texture[3]&268435456U) && (!(v.texture[3]&8U) || (v.texture[3]&~(268435456U|10U))))
                    throw std::runtime_error("GPU landscape receiver used on a non-tile vertex");
            for(const auto list:{vertices,mesh.line_view()})
                for(const auto& v:list) if(v.texture[3]&65536U) {
                    if(v.texture[3]!=65536U || v.texture[2]!=0 || !std::isfinite(v.billboard[0]) || v.billboard[0]<0)
                        throw std::runtime_error("Invalid source span face attributes");
                    const uint64_t key=(uint64_t(v.texture[0])<<32)|v.texture[1];
                    if(checked_span_headers.insert(key).second && !source_span_payload_valid(mesh.texels,v.texture[0],v.texture[1]))
                        throw std::runtime_error("Invalid source span coverage payload");
                } else if(v.texture[3]&1024U) {
                    const auto start=size_t(v.texture[0]);
                    if((v.texture[3]!=1024U && v.texture[3]!=1026U && v.texture[3]!=1028U && v.texture[3]!=1030U)
                        || v.texture[1]!=15 || v.texture[2]!=15
                        || start>mesh.texels.size() || mesh.texels.size()-start<9)
                        throw std::runtime_error("Invalid packed source-font glyph");
                } else if(v.texture[3]&512U) {
                    if(v.texture[3]!=512U || v.texture[0]!=0 || mesh.texels.size()<384)
                        throw std::runtime_error("Invalid binned connected-grid header");
                    if(!checked_connected_rows) {
                        for(size_t row=0;row<192;++row) {
                            const auto start=size_t(mesh.texels[row*2]),count=size_t(mesh.texels[row*2+1]);
                            if(count>675 || start<384 || start>mesh.texels.size() || count>mesh.texels.size()-start)
                                throw std::runtime_error("Invalid connected-grid row list");
                            for(size_t i=0;i<count;++i) {
                                const auto record=size_t(mesh.texels[start+i]);
                                if(record<384 || record>mesh.texels.size() || mesh.texels.size()-record<5 || mesh.texels[record]>1)
                                    throw std::runtime_error("Invalid connected-grid row primitive");
                                for(size_t word=1;word<5;++word) {
                                    const auto value=int32_t(mesh.texels[record+word]);
                                    if(value< -8192 || value>8191) throw std::runtime_error("Connected-grid coordinate overflow");
                                }
                            }
                        }
                        checked_connected_rows=true;
                    }
                } else if(v.texture[3]&256U) {
                    const auto start=std::size_t(v.texture[0]);
                    if(v.texture[3]!=256U || start>mesh.texels.size() || mesh.texels.size()-start<4)
                        throw std::runtime_error("Invalid GPU connected-grid line payload");
                    for(unsigned word=0;word<4;++word) {
                        const auto coordinate=static_cast<int32_t>(mesh.texels[start+word]);
                        if(coordinate< -8192 || coordinate>8191)
                            throw std::runtime_error("GPU connected-grid endpoint exceeds arithmetic bounds");
                    }
                } else if(v.texture[3]&128U) {
                    const auto start=std::size_t(v.texture[0]);
                    if((v.texture[3]&121U) || !(v.texture[3]&4U) || v.texture[1]>3 || v.texture[2]>1
                        || start>mesh.texels.size() || mesh.texels.size()-start<268)
                        throw std::runtime_error("Invalid GPU dust payload");
                    for(float coordinate:v.position)
                        if(!std::isfinite(coordinate) || coordinate< -32768 || coordinate>32767
                            || coordinate!=std::trunc(coordinate))
                            throw std::runtime_error("Invalid GPU source dust point");
                    if(checked_dust_words!=start) {
                        for(unsigned word=0;word<268;++word) {
                            if(word>=3 && word<12) {
                                const auto value=static_cast<int32_t>(mesh.texels[start+word]);
                                if(value< -32768 || value>32767) throw std::runtime_error("Invalid GPU dust matrix");
                            } else {
                                const float value=std::bit_cast<float>(mesh.texels[start+word]);
                                if(!std::isfinite(value) || (word<3?std::abs(value)>65536.F:(value<0 || value>1)))
                                    throw std::runtime_error("Invalid GPU dust camera/colour");
                            }
                        }
                        checked_dust_words=start;
                    }
                } else if(v.texture[3]&64U) {
                    const auto start=std::size_t(v.texture[0]);
                    const float low_z=(v.texture[3]&8192U)?-24.F:0.F;
                    if((v.texture[3]&57U) || !(v.texture[3]&4U)
                        || start>mesh.texels.size() || mesh.texels.size()-start<12
                        || v.texture[1]>1 || v.position[0]<0 || v.position[0]>14
                        || v.position[2]<low_z || v.position[2]>14
                        || !std::isfinite(v.position[0]) || !std::isfinite(v.position[2])
                        || v.position[0]!=std::trunc(v.position[0]) || v.position[2]!=std::trunc(v.position[2]))
                        throw std::runtime_error("Invalid GPU source grid payload");
                    if(checked_grid_words!=start) {
                        for(unsigned word=0;word<12;++word) {
                            const auto value=static_cast<int32_t>(mesh.texels[start+word]);
                            if(value< -32768 || value>32767) throw std::runtime_error("Invalid GPU grid source word");
                        }
                        checked_grid_words=start;
                    }
                } else if(v.texture[3]&8U) {
                    if(v.texture[3]&268435456U) {
                        const auto receiver=size_t(v.texture[0])+272+16384;
                        if((v.texture[3]&~(268435456U|10U)) || receiver>=mesh.texels.size())
                            throw std::runtime_error("Invalid GPU landscape receiver payload");
                        const auto height=std::bit_cast<float>(mesh.texels[receiver]);
                        if(!std::isfinite(height) || height>=0 || height< -8.F)
                            throw std::runtime_error("Invalid GPU landscape receiver height");
                    }
                    if(v.texture[3]&48U) throw std::runtime_error("Conflicting tile/sprite/solid payload flags");
                    const auto start=std::size_t(v.texture[0]);
                    if(start>mesh.texels.size() || mesh.texels.size()-start<272+16384)
                        throw std::runtime_error("Truncated GPU tile background payload");
                    const auto bpp=mesh.texels[start+5];
                    const auto scanlines=mesh.texels[start+10];
                    if(mesh.texels[start+12]>2)
                        throw std::runtime_error("Invalid GPU offset-per-tile mode");
                    if(mesh.texels[start+13]>15)
                        throw std::runtime_error("Invalid GPU background attenuation");
                    if(mesh.texels[start+14]>256)
                        throw std::runtime_error("Invalid GPU tunnel border control");
                    const auto unique_rows=(mesh.texels[start+15]>>8)&65535U;
                    if(unique_rows>224 && unique_rows!=512)
                        throw std::runtime_error("Invalid GPU tile coverage controls");
                    if((mesh.texels[start+15]&64U)!=0) {
                        const auto atlas_width=((mesh.texels[start+2]&1U)?64U:32U)
                            *(mesh.texels[start+8]?16U:8U);
                        if(atlas_width!=512U && atlas_width!=1024U)
                            throw std::runtime_error("Unsupported unique landscape atlas width");
                    }
                    if(scanlines>3 || (scanlines && mesh.texels.size()-start<272+16384+448))
                        throw std::runtime_error("Invalid GPU scanline payload");
                    // Word 7 packs the two-bit priority and the selective
                    // face-planet continuation flag. Reject all other bits.
                    if((bpp!=2 && bpp!=4 && bpp!=8) || mesh.texels[start+2]>3
                        || (mesh.texels[start+7]&~256U)>2
                        || mesh.texels[start+8]>1 || mesh.texels[start+9]>16 || mesh.texels[start+11]>1)
                        throw std::runtime_error("Invalid GPU tile background controls");
                } else if(v.texture[3]&16U) {
                    if(v.texture[3]&32U) throw std::runtime_error("Conflicting sprite/solid payload flags");
                    const auto start=std::size_t(v.texture[0]);
                    if(start>mesh.texels.size() || mesh.texels.size()-start<272+16384)
                        throw std::runtime_error("Truncated GPU sprite payload");
                    const auto size=v.texture[2]>>8;
                    if(v.texture[1]>0x1ffffU
                        || (size!=8 && size!=16 && size!=32 && size!=64) || mesh.texels[start+13]>15)
                        throw std::runtime_error("Invalid GPU sprite controls");
                } else if(v.texture[3]&32U) {
                    const auto start=std::size_t(v.texture[0]);
                    if(start>mesh.texels.size() || mesh.texels.size()-start<272
                        || v.texture[1]>255 || mesh.texels[start+13]>15)
                        throw std::runtime_error("Invalid GPU solid palette payload");
                } else if(v.texture[3]&1073741824U) {
                    const uint64_t width=uint64_t(v.texture[1])+1,height=uint64_t(v.texture[2])+1;
                    if(width>16384 || height>16384 || uint64_t(v.texture[0])+((width+3)/4)*height>mesh.texels.size())
                        throw std::runtime_error("Packed shadow mask reference out of bounds");
                } else if(v.texture[3]&1U) {
                    const auto width=uint64_t(v.texture[1])+1,height=uint64_t(v.texture[2])+1;
                    // Source texture masks are 8-bit. Reject before multiplication.
                    const uint64_t palette_words=(v.texture[3]&536870912U)?256:0;
                    const auto pixel_words=palette_words?(width*height+3)/4:width*height;
                    if(width>256 || height>256 || uint64_t(v.texture[0])+palette_words+pixel_words>mesh.texels.size())
                        throw std::runtime_error("Native scene texture reference out of bounds");
                }
        }
        auto next=std::make_unique<State>();next->items.reserve(packets.size());
        next->device=device;next->get=get;next->pass=pass;
        next->depth_test=depth_test;
        next->keyed=!object_keys.empty();
        const bool compatible=state_ && state_->device==device && state_->get==get && state_->pass==pass;
        if(compatible && state_->depth_test==depth_test) next->pipelines=state_->pipelines;
        std::unordered_map<uint32_t,const State::Item*> prior;
        if(compatible && next->keyed && state_->keyed) {
            prior.reserve(state_->items.size());
            for(const auto& item:state_->items) prior.emplace(item.key,&item);
        }
        const uint32_t transparent=0;
        for(std::size_t packet_index=0;packet_index<packets.size();++packet_index) {
            const auto& packet=packets[packet_index];
            const auto& mesh=packet.geometry;
            const auto vertices=mesh.vertex_view();
            if(vertices.empty() && mesh.line_view().empty()) continue;
            const auto index=next->items.size();
            const uint32_t key=next->keyed?object_keys[packet_index]:0;
            const State::Item* candidate=nullptr;
            if(next->keyed) {
                const auto found=prior.find(key);
                if(found!=prior.end()) candidate=found->second;
            } else if(compatible && !state_->keyed && index<state_->items.size()) candidate=&state_->items[index];
            if(candidate && same_draw_geometry(std::span<const DrawPacket>(&candidate->geometry->source,1),std::span<const DrawPacket>(&packet,1))) {
                next->items.push_back({packet.model,candidate->geometry,key});++next->reused;continue;
            }
            auto item=std::make_shared<State::Geometry>();item->source=packet;
            item->ray_positions=std::all_of(mesh.vertex_view().begin(),mesh.vertex_view().end(),[](const auto& vertex) {
                return vertex.visibility_enabled!=2 && !(vertex.texture[3]&~0x28000007U);
            });
            const auto texels=mesh.texels.empty()?std::span<const uint32_t>(&transparent,1):std::span<const uint32_t>(mesh.texels);
            if(candidate && candidate->geometry->source.geometry.texels==mesh.texels)
                item->textures=candidate->geometry->textures;
            else {
                item->textures=std::make_shared<VulkanSceneTextures>();
                if(!item->textures->initialize(device,get,memory,texels)) throw std::runtime_error(item->textures->status());
                ++next->texture_uploads;
            }
            const auto prior_vertices=candidate?candidate->geometry->source.geometry.vertex_view():std::span<const SceneVertex>{};
            if(candidate && prior_vertices.size()==vertices.size()
                && (prior_vertices.data()==vertices.data() || std::equal(vertices.begin(),vertices.end(),prior_vertices.begin())))
                item->triangles=candidate->geometry->triangles;
            else if(!vertices.empty()) {
                item->triangles=std::make_shared<VulkanSceneBuffer>();
                if(!item->triangles->initialize(device,get,memory,vertices)) throw std::runtime_error(item->triangles->status());
                ++next->vertex_uploads;
            }
            const auto lines=mesh.line_view();
            const auto prior_lines=candidate?candidate->geometry->source.geometry.line_view():std::span<const SceneVertex>{};
            if(candidate && prior_lines.size()==lines.size()
                && (prior_lines.data()==lines.data() || std::equal(lines.begin(),lines.end(),prior_lines.begin())))
                item->lines=candidate->geometry->lines;
            else if(!lines.empty()) {
                item->lines=std::make_shared<VulkanSceneBuffer>();
                if(!item->lines->initialize(device,get,memory,lines)) throw std::runtime_error(item->lines->status());
                ++next->vertex_uploads;
            }
            next->items.push_back({packet.model,std::move(item),key});++next->uploaded;
        }
        if(!next->items.empty() && !next->pipelines) {
            auto pipelines=std::make_shared<State::Pipelines>();
            if(!pipelines->layout.initialize(device,get,memory,std::span<const uint32_t>(&transparent,1))) throw std::runtime_error(pipelines->layout.status());
            next->pipelines=std::move(pipelines);
        }
        // Most HUD/background passes never draw lines. Compile only topologies
        // actually present, retaining each pipeline when later packets change.
        for(const auto& item:next->items) {
            auto& pipelines=*next->pipelines;
            const auto layout=pipelines.layout.layout();
            if(item.geometry->triangles && !pipelines.triangles_ready) {
                if(!pipelines.triangles.initialize(device,get,pass,depth_test,SceneTopology::triangles,layout,SceneBlend::opaque,cache_))
                    throw std::runtime_error(pipelines.triangles.status());
                pipelines.triangles_ready=true;
            }
            if(item.geometry->lines && !pipelines.lines_ready) {
                if(!pipelines.lines.initialize(device,get,pass,depth_test,SceneTopology::lines,layout,SceneBlend::opaque,cache_))
                    throw std::runtime_error(pipelines.lines.status());
                pipelines.lines_ready=true;
            }
        }
        if(next->keyed) {
            next->ray_items.reserve(next->items.size());
            for(size_t i=0;i<next->items.size();++i) next->ray_items.emplace(next->items[i].key,i);
        }
        status_="Native scene uploaded";state_=std::move(next);return true;
    } catch(const std::exception& e) {status_=e.what();return false;}
}
bool VulkanDrawPackets::record(VkCommandBuffer commands,VkExtent2D extent,const EyeCamera& camera) const {
    return record_range(commands,extent,camera,0,size());
}
bool VulkanDrawPackets::record_range(VkCommandBuffer commands,VkExtent2D extent,const EyeCamera& camera,
    std::size_t first,std::size_t count) const {
    if(!state_ || !commands || !extent.width || !extent.height) return false;
    if(first>state_->items.size() || count>state_->items.size()-first) return false;
    const auto items=std::span(state_->items).subspan(first,count);
    for(const auto& item:items) if(!model_eye_camera(camera,item.model)) return false;
    for(const auto& item:items) {
        const auto& geometry=*item.geometry;
        auto draw_camera=camera;
        if(geometry.source.preserve_native_colour || (state_->keyed && is_source_shadow_pass(item.key))) draw_camera.effects={};
        if(geometry.triangles && !state_->pipelines->triangles.record_model(commands,extent,geometry.triangles->buffer(),
            geometry.triangles->count(),draw_camera,item.model,geometry.textures->descriptor())) return false;
        if(geometry.lines && !state_->pipelines->lines.record_model(commands,extent,geometry.lines->buffer(),
            geometry.lines->count(),draw_camera,item.model,geometry.textures->descriptor())) return false;
    }
    return true;
}
}
