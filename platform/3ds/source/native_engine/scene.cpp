#include "scene.hpp"
#include <algorithm>
#include <array>
#include <cstring>
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
        if (!last.slanted && last.even==r.even && last.odd==r.odd && last.scale==r.scale) {
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
bool Scene::append_edge_run(const EdgeRun& c, std::uint8_t even,
    std::uint8_t odd, std::uint16_t scale, int width, int height) {
    // PICA quantizes corners to sixteenths. Certify each row centre against
    // the source's inclusive integer span. Split only bands whose quantized
    // side would include/drop a pixel; those rows use exact rectangle spans.
    //
    // compile() bounds both ends and the steps to +/-1024 pixels (2^18 in
    // 8.8), so an edge at any row, sixteen times it, and that plus one step
    // all fit 32 bits: no 64-bit arithmetic is needed on the ARM11. The
    // corner is floor(fine / 256), which is an arithmetic shift.
    const auto corner=[](std::int32_t edge,std::int32_t step,std::int32_t offset) {
        return std::int16_t(((edge*16+step)>>8)+offset);
    };
    const std::int32_t clip_left=std::max<std::int32_t>(0,c.left);
    const std::int32_t clip_right=std::min<std::int32_t>(width,c.right);
    const std::int32_t u=c.u,v=c.v,du=c.du,dv=c.dv;
    const auto span=[&](std::int32_t row) {
        const auto y=c.top+std::int32_t(row);
        if (y<0 || y>=height) return true;
        const auto first=std::int32_t((u+du*row)>>8),last=std::int32_t((v+dv*row)>>8);
        return append({std::int16_t(std::max(clip_left,first)),std::int16_t(y),
            std::int16_t(std::min(clip_right,last+1)),std::int16_t(y+1),even,odd,scale});
    };
    std::int32_t first=0, final=c.bottom-c.top-1;
    while (first<=final) {
        auto last=final;
        // The top corners of a band depend on its first row alone.
        const auto top_left=corner(u+du*first,-du,-7);
        const auto top_right=corner(v+dv*first,-dv,9);
        const auto top_y=std::int16_t((c.top+first)*16+7);
        const std::int32_t source_left_first=u+du*first, source_right_first=v+dv*first;
        for (;;) {
            if (last<=first) {
                if (!span(first)) return false;
                ++first;
                break;
            }
            const auto bottom_left=corner(u+du*last,du,-7);
            const auto bottom_right=corner(v+dv*last,dv,9);
            const auto bottom_y=std::int16_t((c.top+last)*16+9);
            const std::int32_t dy=bottom_y-top_y;
            std::int32_t bad=last+1;
            // Sides that cross fail at the band's first row whatever else holds.
            if (top_left>top_right || bottom_left>bottom_right) bad=first;
            else {
                // Here 0 < y < dy <= 32754 and every corner fits int16_t, so
                // each interpolated edge fits int32_t. Advance its numerator
                // once per row instead of multiplying 64-bit values.
                std::int32_t left=std::int32_t(top_left)*(dy-1)+bottom_left;
                std::int32_t right=std::int32_t(top_right)*(dy-1)+bottom_right;
                const std::int32_t left_step=(bottom_left-top_left)*16;
                const std::int32_t right_step=(bottom_right-top_right)*16;
                std::int32_t source_left=source_left_first, source_right=source_right_first;
                for (auto row=first; row<=last; ++row) {
                    const auto want_left=source_left>>8;
                    const auto want_right=source_right>>8;
                    if (want_left < clip_left || want_right >= clip_right
                        || left<=(want_left*16-8)*dy || left>=(want_left*16+8)*dy
                        || right<=(want_right*16+8)*dy || right>=(want_right*16+24)*dy) {
                        bad=row; break;
                    }
                    left+=left_step; right+=right_step;
                    source_left+=du; source_right+=dv;
                }
            }
            if (bad<=last) { last=bad-1; continue; }
            if (rectangles_.size()==capacity) { stats_.overflow=true; return false; }
            auto& r=rectangles_.emplace_back();
            r.left=std::int16_t(clip_left);
            r.top=std::int16_t(std::max<std::int64_t>(0,c.top+first));
            r.right=std::int16_t(clip_right);
            r.bottom=std::int16_t(std::min<std::int64_t>(height,c.top+last+1));
            r.even=even; r.odd=odd; r.scale=scale;
            r.top_left=top_left; r.top_right=top_right;
            r.bottom_left=bottom_left; r.bottom_right=bottom_right;
            r.top_y=top_y; r.bottom_y=bottom_y;
            r.slanted=true;
            first=last+1;
            break;
        }
    }
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

// The spans sample() yields over one clipped texture command, row by row,
// without asking it for every pixel. A texture command samples U from the
// column alone and V from the row alone (a textured span row: both from the
// column), so the column's texel index is worked out once for the whole
// command and each row then only reads texels. Every value is the one
// sample() computes; the divisions and wrapping products it repeats per
// pixel are done per column or per row here. Returns false when the command
// is outside what this handles exactly, leaving it to the per-pixel loop.
bool Scene::append_texture_spans(const render::RasterCommands& source,
    const render::RasterCommand& c, int left, int top, int right, int bottom, bool& ok) {
    constexpr std::size_t maximum_columns=512;
    constexpr std::uint16_t outside=0xffffU;
    if (c.textured<1U || c.textured>3U || c.u_mask>255U || c.v_mask>255U
        || std::size_t(right-left)>maximum_columns) return false;
    const std::uint32_t texture_width=c.u_mask+1U, texture_height=c.v_mask+1U;
    // The whole texture lies inside the texel store: no index can leave it.
    if (c.texture_offset>source.texels.size()
        || std::size_t(texture_width)*texture_height>source.texels.size()-c.texture_offset)
        return false;
    const auto* const texels=source.texels.data()+c.texture_offset;
    const std::uint32_t columns=std::uint32_t(right-left);
    const std::uint32_t rows=std::uint32_t(bottom-top);
    ok=true;
    if (c.textured==1U) {
        // Both coordinates step along the row; every row repeats the first.
        std::array<std::uint8_t,maximum_columns> colours;
        std::array<bool,maximum_columns> opaque;
        const auto first=std::uint32_t(left-c.left);
        std::uint32_t u=(std::uint32_t(c.u)+std::uint32_t(c.du)*first)&65535U;
        std::uint32_t v=(std::uint32_t(c.v)+std::uint32_t(c.dv)*first)&65535U;
        for (std::uint32_t column=0;column<columns;++column) {
            const auto texel=texels[(((v>>8U)+c.reserved1)&c.v_mask)*texture_width
                +(((u>>8U)+c.reserved0)&c.u_mask)];
            opaque[column]=texel!=0U;
            colours[column]=std::uint8_t(c.colour_base+texel);
            u=(u+std::uint32_t(c.du))&65535U;
            v=(v+std::uint32_t(c.dv))&65535U;
        }
        for (std::uint32_t row=0;row<rows;++row) {
            const auto y=top+int(row);
            stats_.sampled_pixels+=columns;
            for (std::uint32_t column=0;column<columns;) {
                if (!opaque[column]) { ++column; continue; }
                const auto begin=column;
                const auto colour=colours[column];
                while (++column<columns && opaque[column] && colours[column]==colour) {}
                if (!append({std::int16_t(left+int(begin)),std::int16_t(y),
                        std::int16_t(left+int(column)),std::int16_t(y+1),colour,colour,1})) {
                    ok=false; return true;
                }
            }
        }
        return true;
    }
    // Sprites: this path takes the unscaled raster only (dv is the draw
    // scale), where a column or row index is the coordinate difference.
    if (c.dv!=1) return false;
    std::array<std::uint16_t,maximum_columns> texel_column;
    const bool scaled=c.textured==2U;
    if (scaled) {
        // u = (x - c.u) * width / c.du, for the non-negative differences the
        // recorder produces; the products stay far below 2^32.
        if (c.du<=0 || left<c.u || top<c.v
            || std::int64_t(right)-c.u>65536 || std::int64_t(bottom)-c.v>65536) return false;
        const auto divisor=std::uint32_t(c.du);
        std::uint32_t numerator=std::uint32_t(left-c.u)*texture_width;
        std::uint32_t quotient=numerator/divisor, remainder=numerator%divisor;
        for (std::uint32_t column=0;column<columns;++column) {
            texel_column[column]=quotient>c.u_mask?outside:std::uint16_t(quotient);
            remainder+=texture_width;
            while (remainder>=divisor) { remainder-=divisor; ++quotient; }
        }
    } else {
        const auto first=std::uint32_t(left-c.left);
        std::uint32_t u=(std::uint32_t(c.u)+std::uint32_t(c.du)*first)&65535U;
        for (std::uint32_t column=0;column<columns;++column) {
            const auto index=u>>8U;
            texel_column[column]=index>c.u_mask?outside:std::uint16_t(index);
            u=(u+std::uint32_t(c.du))&65535U;
        }
    }
    const bool fixed_colour=scaled && (c.reserved0&256U)!=0U;
    const auto fixed=std::uint8_t(c.reserved0);
    for (std::uint32_t row=0;row<rows;++row) {
        const auto y=top+int(row);
        stats_.sampled_pixels+=columns;
        const std::uint32_t v=scaled
            ? std::uint32_t(y-c.v)*texture_height/std::uint32_t(c.du)
            : ((std::uint32_t(c.v)+std::uint32_t(c.du)*std::uint32_t(y-c.top))&65535U)>>8U;
        if (v>c.v_mask) continue;
        const auto* const line=texels+std::size_t(v)*texture_width;
        for (std::uint32_t column=0;column<columns;) {
            const auto index=texel_column[column];
            const auto texel=index==outside?std::uint8_t{}:line[index];
            if (texel==0U) { ++column; continue; }
            const auto begin=column;
            const auto colour=fixed_colour?fixed:std::uint8_t(c.colour_base+texel);
            for (++column;column<columns;++column) {
                const auto next_index=texel_column[column];
                const auto next=next_index==outside?std::uint8_t{}:line[next_index];
                if (next==0U || (fixed_colour?fixed:std::uint8_t(c.colour_base+next))!=colour) break;
            }
            if (!append({std::int16_t(left+int(begin)),std::int16_t(y),
                    std::int16_t(left+int(column)),std::int16_t(y+1),colour,colour,1})) {
                ok=false; return true;
            }
        }
    }
    return true;
}

// One recorded command, in the order and with the tests of the unpacked
// stream. `left`..`bottom` are its bounds clipped to the raster.
bool Scene::compile_command(const render::RasterCommands& source,
    const render::RasterCommand& c, int width, int height, std::uint32_t uniform_scale) {
    const int left=std::max<std::int32_t>(0,c.left), top=std::max<std::int32_t>(0,c.top);
    const int right=std::min<std::int32_t>(width,c.right), bottom=std::min<std::int32_t>(height,c.bottom);
    if (left>=right || top>=bottom) return true;
    if (!c.textured && !(c.reserved1&5U) && c.reserved0<=65535U) {
        const auto even=std::uint8_t(c.even);
        const bool uniform=!c.dither || c.reserved0>=uniform_scale;
        const auto odd=std::uint8_t(uniform?c.even:c.odd);
        const auto scale=std::uint16_t(uniform?1U:std::max<std::uint32_t>(1U,c.reserved0));
        if (c.reserved1&8U)
            return compile_edge_run({c.left,c.top,c.right,c.bottom,c.u,c.v,c.du,c.dv},
                even,odd,scale,width,height);
        return append({std::int16_t(left),std::int16_t(top),std::int16_t(right),std::int16_t(bottom),
            even,odd,scale});
    }
    bool ok=true;
    if (append_texture_spans(source,c,left,top,right,bottom,ok)) return ok;
    // Mask and edge-only spans, and any texture command outside the
    // fast path's bounds, retain exact per-pixel sampling.
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
    return true;
}

bool Scene::compile_edge_run(const EdgeRun& c, std::uint8_t even, std::uint8_t odd,
    std::uint16_t scale, int width, int height) {
    // Corners must fit the vertex format: a 16-bit count of
    // sixteenths. The recorder clamps polygons to the raster.
    const auto rows=c.bottom-c.top;
    const auto fits=[](std::int64_t fixed) {
        return fixed>-(std::int64_t{1024}<<8) && fixed<(std::int64_t{1024}<<8);
    };
    if (c.top<-1024 || c.bottom>1024
        || !fits(c.u) || !fits(c.v) || !fits(c.du) || !fits(c.dv)
        || !fits(std::int64_t{c.u}+std::int64_t{c.du}*rows)
        || !fits(std::int64_t{c.v}+std::int64_t{c.dv}*rows)) {
        stats_.overflow=true; return false;
    }
    return append_edge_run(c,even,odd,scale,width,height);
}

bool Scene::compile(const render::RasterCommands& source) {
    rectangles_.clear(); stats_={}; stats_.source_commands=source.command_count();
    if (source.width()>4096 || source.height()>4096) { stats_.overflow=true; return false; }
    const int width=int(source.width()), height=int(source.height());
    const std::uint32_t uniform_scale=std::max(source.width(),source.height());
    if (!source.packed()) {
        for (const auto& c:source.commands)
            if (!compile_command(source,c,width,height,uniform_scale)) return false;
        stats_.rectangles=rectangles_.size(); return true;
    }
    // The packed stream: the same commands in the same order. A plain
    // rectangle or an edge run is compiled straight from its few words; what
    // was kept whole goes through compile_command.
    using Stream=render::RasterCommands;
    const auto words=source.packed_words();
    for (std::size_t at=0;at<words.size();) {
        const auto* word=words.data()+at;
        const auto kind=word[0]&3U;
        if (kind==Stream::packed_whole) {
            render::RasterCommand c;
            std::memcpy(static_cast<void*>(&c),word+1,sizeof(c));
            at+=Stream::packed_whole_words;
            if (!compile_command(source,c,width,height,uniform_scale)) return false;
            continue;
        }
        const std::int32_t c_left=std::int16_t(word[1]), c_top=std::int16_t(word[1]>>16U);
        const std::int32_t c_right=std::int16_t(word[2]), c_bottom=std::int16_t(word[2]>>16U);
        const bool run=kind==Stream::packed_run;
        at+=run?Stream::packed_run_words:Stream::packed_solid_words;
        const int left=std::max<std::int32_t>(0,c_left), top=std::max<std::int32_t>(0,c_top);
        const int right=std::min<std::int32_t>(width,c_right), bottom=std::min<std::int32_t>(height,c_bottom);
        if (left>=right || top>=bottom) continue;
        const auto even=std::uint8_t(word[0]>>8U);
        const std::uint32_t dither_scale=run?0U:word[0]>>24U;
        const bool uniform=!(word[0]&4U) || dither_scale>=uniform_scale;
        const auto odd=uniform?even:std::uint8_t(word[0]>>16U);
        const auto scale=std::uint16_t(uniform?1U:std::max<std::uint32_t>(1U,dither_scale));
        if (run) {
            if (!compile_edge_run({c_left,c_top,c_right,c_bottom,std::int32_t(word[3]),
                    std::int32_t(word[4]),std::int32_t(word[5]),std::int32_t(word[6])},
                    even,odd,scale,width,height)) return false;
        } else if (!append({std::int16_t(left),std::int16_t(top),std::int16_t(right),
                std::int16_t(bottom),even,odd,scale})) return false;
    }
    stats_.rectangles=rectangles_.size(); return true;
}
} // namespace starfox::platform_3ds::native_engine
