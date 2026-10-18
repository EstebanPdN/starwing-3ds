#include "scene.hpp"
#include <algorithm>
#include <limits>

namespace starfox::platform_3ds::native_engine {
bool Scene::append(Rectangle r) {
    if (r.left >= r.right || r.top >= r.bottom) return true;
    while (!rectangles_.empty()) {
        const auto& last=rectangles_.back();
        if (r.left>last.left || r.top>last.top || r.right<last.right || r.bottom<last.bottom) break;
        rectangles_.pop_back();
    }
    stats_.covered_pixels += std::size_t(r.right-r.left)*std::size_t(r.bottom-r.top);
    if (!rectangles_.empty()) {
        auto& last=rectangles_.back();
        if (last.even==r.even && last.odd==r.odd && last.scale==r.scale) {
            if (last.left==r.left && last.right==r.right && last.bottom==r.top) {
                last.bottom=r.bottom; return true;
            }
            if (last.top==r.top && last.bottom==r.bottom && last.right==r.left) {
                last.right=r.right; return true;
            }
        }
    }
    if (rectangles_.size() == capacity) { stats_.overflow = true; return false; }
    rectangles_.push_back(r);
    return true;
}
std::optional<std::uint8_t> Scene::sample(const render::RasterCommands& b,
    const render::RasterCommand& c, int x, int y) noexcept {
    if (!c.textured) {
        if ((c.reserved1&1U) && x!=c.u && x!=c.v) return {};
        if (c.reserved1&4U) {
            const auto stride=c.u_mask;
            if (!stride || (stride&3U) || (c.texture_offset&3U)
                || unsigned(y)>=c.v_mask || unsigned(x)/32>=stride/4
                || c.texture_offset>b.texels.size()
                || c.v_mask>(b.texels.size()-c.texture_offset)/stride) return {};
            const auto at=std::size_t(c.texture_offset)+std::size_t(y)*stride+std::size_t(x/32)*4;
            const auto bits=std::uint32_t(b.texels[at])|(std::uint32_t(b.texels[at+1])<<8)
                |(std::uint32_t(b.texels[at+2])<<16)|(std::uint32_t(b.texels[at+3])<<24);
            if (!(bits&(1U<<(x&31)))) return {};
        }
        const auto scale=int(std::max<std::uint32_t>(1U,c.reserved0));
        return std::uint8_t(c.dither && (((x/scale)^(y/scale))&1) ? c.odd:c.even);
    }
    const auto dx=std::uint32_t(x-c.left);
    std::uint32_t u{},v{};
    if (c.textured==1) {
        u=(((((std::uint32_t(c.u)+std::uint32_t(c.du)*dx)&65535U)>>8U)+c.reserved0)&c.u_mask);
        v=(((((std::uint32_t(c.v)+std::uint32_t(c.dv)*dx)&65535U)>>8U)+c.reserved1)&c.v_mask);
    } else if (c.textured==2) {
        if (c.dv<=0 || c.du<=0) return {};
        u=std::uint32_t((x-c.u)/c.dv)*(c.u_mask+1)/c.du;
        v=std::uint32_t((y-c.v)/c.dv)*(c.v_mask+1)/c.du;
    } else if (c.textured==3) {
        if (c.dv<=0) return {};
        u=((std::uint32_t(c.u)+std::uint32_t(c.du)*(dx/c.dv))&65535U)>>8U;
        v=((std::uint32_t(c.v)+std::uint32_t(c.du)*(std::uint32_t(y-c.top)/c.dv))&65535U)>>8U;
    } else return {};
    if (u>c.u_mask || v>c.v_mask) return {};
    const auto at=std::uint64_t(c.texture_offset)+std::uint64_t(v)*(std::uint64_t(c.u_mask)+1)+u;
    if (at>=b.texels.size() || !b.texels[std::size_t(at)]) return {};
    return std::uint8_t(c.textured==2 && (c.reserved0&256U)
        ? c.reserved0 : c.colour_base+b.texels[std::size_t(at)]);
}
bool Scene::compile(const render::RasterCommands& source) {
    rectangles_.clear(); stats_={}; stats_.source_commands=source.commands.size();
    if (source.width()>4096 || source.height()>4096) { stats_.overflow=true; return false; }
    for (const auto& c:source.commands) {
        const int left=std::max<std::int32_t>(0,c.left), top=std::max<std::int32_t>(0,c.top);
        const int right=std::min<std::int32_t>(source.width(),c.right), bottom=std::min<std::int32_t>(source.height(),c.bottom);
        if (left>=right || top>=bottom) continue;
        if (!c.textured && !(c.reserved1&5U) && c.reserved0<=65535U) {
            const auto even=std::uint8_t(c.even);
            const bool uniform=!c.dither || c.reserved0>=std::max(source.width(),source.height());
            const auto odd=std::uint8_t(uniform?c.even:c.odd);
            if (!append({std::int16_t(left),std::int16_t(top),std::int16_t(right),std::int16_t(bottom),
                    even,odd,std::uint16_t(uniform?1U:std::max<std::uint32_t>(1U,c.reserved0))})) return false;
        } else {
            // Uncommon texture/mask spans retain exact integer sampling.
            for (int y=top;y<bottom;++y) {
                int begin=left; std::optional<std::uint8_t> previous;
                for (int x=left;x<=right;++x) {
                    const auto colour=x<right?sample(source,c,x,y):std::optional<std::uint8_t>{};
                    if (x<right) ++stats_.sampled_pixels;
                    if (colour!=previous) {
                        if (previous && !append({std::int16_t(begin),std::int16_t(y),std::int16_t(x),std::int16_t(y+1),*previous,*previous,1})) return false;
                        begin=x;previous=colour;
                    }
                }
            }
        }
    }
    stats_.rectangles=rectangles_.size(); return true;
}
} // namespace starfox::platform_3ds::native_engine
