#include "starfox/render/gpu_raster.hpp"
#include <cstring>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace starfox::render {
void replay_raster_commands(const RasterCommands& batch,Framebuffer& frame,SurfaceBuffer* surfaces,bool clear_target) {
    frame.record_to(nullptr);
    if(clear_target) {frame.clear();if(surfaces) surfaces->clear();}
    for(const auto& c:batch.commands) {
        const bool edge_run=c.textured==0 && (c.reserved1&8U)!=0;
        for(std::int32_t y=std::max<std::int32_t>(0,c.top);
            y<std::min<std::int32_t>(int(batch.height()),c.bottom);++y)
            for(std::int32_t x=std::max<std::int32_t>(0,c.left);
                x<std::min<std::int32_t>(int(batch.width()),c.right);++x) {
                if(edge_run) {
                    // Arithmetic shifts: an edge may start left of pixel zero.
                    const auto row=y-c.top;
                    if(x<((c.u+c.du*row)>>8) || x>((c.v+c.dv*row)>>8)) continue;
                } else
                if(c.textured==0 && (c.reserved1&1U)!=0 && x!=c.u && x!=c.v) continue;
                if(c.textured==0 && (c.reserved1&4U)!=0) {
                    const auto stride=c.u_mask;
                    if(!stride || (stride&3U) || (c.texture_offset&3U)
                        || std::uint32_t(y)>=c.v_mask || std::uint32_t(x)/32>=stride/4
                        || c.texture_offset>batch.texels.size()
                        || c.v_mask>(batch.texels.size()-c.texture_offset)/stride) continue;
                    const auto at=std::size_t(c.texture_offset)+std::size_t(y)*stride+std::size_t(x/32)*4;
                    const auto bits=std::uint32_t(batch.texels[at])|(std::uint32_t(batch.texels[at+1])<<8)
                        |(std::uint32_t(batch.texels[at+2])<<16)|(std::uint32_t(batch.texels[at+3])<<24);
                    if((bits&(1U<<(x&31)))==0) continue;
                }
                const auto dither_scale=int(std::max<std::uint32_t>(1U,c.reserved0));
                auto colour=c.dither && (((x/dither_scale)^(y/dither_scale))&1)?c.odd:c.even;
                if(c.textured) {
                    const auto dx=std::uint32_t(x-c.left);
                    std::uint32_t u,v;
                    if(c.textured==1) {
                        u=(((((std::uint32_t(c.u)+std::uint32_t(c.du)*dx)&65535U)>>8U)+c.reserved0)&c.u_mask);
                        v=(((((std::uint32_t(c.v)+std::uint32_t(c.dv)*dx)&65535U)>>8U)+c.reserved1)&c.v_mask);
                    } else if(c.textured==2) {
                        u=std::uint32_t((x-c.u)/c.dv)*(c.u_mask+1)/c.du;
                        v=std::uint32_t((y-c.v)/c.dv)*(c.v_mask+1)/c.du;
                    } else {
                        u=((std::uint32_t(c.u)+std::uint32_t(c.du)*(dx/c.dv))&65535U)>>8U;
                        v=((std::uint32_t(c.v)+std::uint32_t(c.du)*(std::uint32_t(y-c.top)/c.dv))&65535U)>>8U;
                        if(u>c.u_mask || v>c.v_mask) continue;
                    }
                    const auto texel=batch.texels[c.texture_offset+v*(c.u_mask+1)+u];
                    if(!texel) continue;
                    colour=std::uint8_t(c.colour_base+texel);
                    if(c.textured==2 && (c.reserved0&256)) colour=std::uint8_t(c.reserved0);
                }
                frame.set_stored(x,y,std::uint8_t(colour),PixelLayer(c.tag));
                if(surfaces && c.has_surface) surfaces->set(x,y,
                    {c.surface[0],c.surface[1],c.surface[2],c.surface[3]},std::uint8_t(colour));
            }
    }
}
struct GpuRaster::Impl {std::string status{"GPU native raster unavailable on this build"};};
GpuRaster::GpuRaster():impl_(std::make_unique<Impl>()) {}
GpuRaster::~GpuRaster()=default;
void GpuRaster::release_device() noexcept {impl_.reset();}
const std::string& GpuRaster::status() const {
    static const std::string released{"GPU raster device released"};return impl_?impl_->status:released;
}
bool GpuRaster::render(RasterCommands& batch,Framebuffer& frame,SurfaceBuffer* surfaces) {
    (void)batch;(void)frame;(void)surfaces;return false;
}
bool GpuRaster::render_resident(void* device,RasterCommands& batch,bool surfaces,bool gpu_binning) {
    (void)device;(void)batch;(void)surfaces;(void)gpu_binning;return false;
}
GpuRasterOutput GpuRaster::enqueue_commands(void* device,void* command,RasterCommands& batch,bool surfaces,bool gpu_binning) {
    return {};
}
GpuRasterOutput GpuRaster::resident_output() const {
    return {};
}
GpuRasterOutput GpuRaster::enqueue_row_spans(void* device,void* command,void* spans,
    std::uint32_t polygon_count,std::uint32_t width,std::uint32_t height,bool surfaces,void* texels,bool pixel_coverage,
    const GpuRasterOutput* background,bool wave_rows,std::int16_t wave_offset,std::uint32_t wave_frame,std::uint32_t texel_bytes,
    const GpuGeometryDepthInput* geometry_depth) {
    return {};
}
bool GpuRaster::readback(Framebuffer& frame,SurfaceBuffer* surfaces) {
    (void)frame;(void)surfaces;return false;
}
bool GpuRaster::wait_for_completion() {
    return false;
}
}
