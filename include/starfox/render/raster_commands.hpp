#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
#include <unordered_map>
namespace starfox::render {
// Ordered native scanline commands. Projection, clipping and BSP selection
// retain source arithmetic; pixel coverage/material sampling can execute on
// either backend without substituting hardware triangle edge conventions.
struct RasterCommand {
    std::int32_t left{},top{},right{},bottom{};
    std::uint32_t even{},odd{},dither{},tag{};
    std::uint32_t texture_offset{},u_mask{},v_mask{},colour_base{};
    std::int32_t u{},v{},du{},dv{};
    std::array<float,4> surface{};
    // Solid GPU row spans: reserved0 is dither scale; reserved1 bit 0 selects
    // edge-only coverage at original X endpoints u/v (not clipped bounds).
    // Bit 1 marks a wave span: du is the signed source phase offset and dv
    // the animation phase. GPU row lookup must explicitly enable wave rows.
    // Bit 2 masks solid coverage using little-endian uint32 rows in texels:
    // texture_offset is aligned byte offset, u_mask is aligned byte stride,
    // v_mask is row count. Bits use absolute stored-frame X/Y coordinates.
    // Textured commands use these fields for texture scroll instead.
    std::uint32_t has_surface{},textured{},reserved0{},reserved1{};
};
static_assert(sizeof(RasterCommand)==96);
class RasterCommands {
public:
    void reset(std::uint32_t width,std::uint32_t height) {
        width_=width;height_=height;commands.clear();texels.clear();textures_.clear();
    }
    void add(RasterCommand command) {
        if(command.right<=0 || command.bottom<=0 || command.left>=int(width_) || command.top>=int(height_)
            || command.left>=command.right || command.top>=command.bottom) return;
        commands.push_back(command);
    }
    std::uint32_t texture(std::span<const std::uint8_t> pixels) {
        const auto found=textures_.find(pixels.data());
        if(found!=textures_.end()) return found->second;
        const auto offset=std::uint32_t(texels.size());
        texels.insert(texels.end(),pixels.begin(),pixels.end());
        textures_.emplace(pixels.data(),offset);return offset;
    }
    void bin_rows() {
        const auto tiles=(width_+63)/64;
        rows.assign(std::size_t(height_)*tiles+1,0);
        const auto visit=[&](const RasterCommand& c,const auto& action) {
            const auto first=unsigned(std::max<std::int32_t>(0,c.left))/64;
            const auto last=(unsigned(std::min<std::int32_t>(int(width_),c.right))+63)/64;
            for(std::int32_t y=std::max<std::int32_t>(0,c.top);
                y<std::min<std::int32_t>(int(height_),c.bottom);++y)
                for(unsigned tile=first;tile<last;++tile) action(unsigned(y)*tiles+tile);
        };
        for(const auto& c:commands) visit(c,[&](unsigned row){++rows[row+1];});
        for(std::size_t row=1;row<rows.size();++row) rows[row]+=rows[row-1];
        indices.resize(rows.back());cursors_=rows;
        for(std::uint32_t i=0;i<commands.size();++i) {
            visit(commands[i],[&](unsigned row){indices[cursors_[row]++]=i;});
        }
    }
    std::uint32_t width() const {return width_;}
    std::uint32_t height() const {return height_;}
    std::vector<RasterCommand> commands;
    std::vector<std::uint8_t> texels;
    std::vector<std::uint32_t> rows,indices;
private:
    std::uint32_t width_{},height_{};
    std::vector<std::uint32_t> cursors_;
    std::unordered_map<const std::uint8_t*,std::uint32_t> textures_;
};
}
