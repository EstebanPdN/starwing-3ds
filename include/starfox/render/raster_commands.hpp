#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
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
    // Bit 3 marks an edge run, the rows of a flat polygon between two edge
    // changes: row r (counted from top) covers the inclusive pixels from
    // (u + du*r) >> 8 to (v + dv*r) >> 8; left/right only bound them. Only
    // recorded for a consumer that asked (RasterCommands::accept_edge_runs).
    // Textured commands use these fields for texture scroll instead.
    std::uint32_t has_surface{},textured{},reserved0{},reserved1{};
};
static_assert(sizeof(RasterCommand)==96);
// A packed stream stores a whole command as its bytes.
static_assert(std::is_trivially_copyable_v<RasterCommand>);
class RasterCommands {
public:
    void reset(std::uint32_t width,std::uint32_t height) {
        width_=width;height_=height;commands.clear();texels.clear();textures_.clear();
        packed_size_=0;packed_count_=0;unpacked_words_=0;last_texture_=nullptr;
        recent_texture_count_=0;
    }
    void add(RasterCommand command) {
        if(rejects(command.left,command.top,command.right,command.bottom)) return;
        if(pack_) {pack_command(command);return;}
        commands.push_back(command);
    }
    // The two commands a scan converter records by the thousand, built in
    // place instead of being constructed, passed by value and copied (96
    // bytes each time). Each is add() of the command with these fields set
    // and every other field zero, add()'s rejection test included.
    //
    // A plain rectangle of one colour pair. `span_surface` stores the
    // default surface sample a recorded polygon span carries (normal +Z).
    void add_solid(std::int32_t left,std::int32_t top,std::int32_t right,std::int32_t bottom,
        std::uint32_t even,std::uint32_t odd,std::uint32_t dither,std::uint32_t tag,
        bool span_surface=false,std::uint32_t dither_scale=0U) {
        if(rejects(left,top,right,bottom)) return;
        if(pack_ && dither_scale<=255U
            && fits_packed(left,top,right,bottom,even,odd,dither,tag)) {
            auto* word=packed_room(packed_solid_words);
            word[0]=packed_solid|(dither<<2U)|(std::uint32_t(span_surface)<<3U)|(tag<<4U)
                |(even<<8U)|(odd<<16U)|(dither_scale<<24U);
            word[1]=std::uint32_t(std::uint16_t(left))|(std::uint32_t(std::uint16_t(top))<<16U);
            word[2]=std::uint32_t(std::uint16_t(right))|(std::uint32_t(std::uint16_t(bottom))<<16U);
            ++packed_count_;
            return;
        }
        RasterCommand command;
        command.left=left;command.top=top;command.right=right;command.bottom=bottom;
        command.even=even;command.odd=odd;command.dither=dither;command.tag=tag;
        command.reserved0=dither_scale;
        if(span_surface) command.surface[2]=1.0F;
        if(pack_) pack_whole(command);
        else commands.push_back(command);
    }
    // An edge run (reserved1 bit 3): see RasterCommand.
    void add_edge_run(std::int32_t left,std::int32_t top,std::int32_t right,std::int32_t bottom,
        std::uint32_t even,std::uint32_t odd,std::uint32_t dither,std::uint32_t tag,
        std::int32_t u,std::int32_t v,std::int32_t du,std::int32_t dv) {
        if(rejects(left,top,right,bottom)) return;
        if(pack_ && fits_packed(left,top,right,bottom,even,odd,dither,tag)) {
            auto* word=packed_room(packed_run_words);
            word[0]=packed_run|(dither<<2U)|(tag<<4U)|(even<<8U)|(odd<<16U);
            word[1]=std::uint32_t(std::uint16_t(left))|(std::uint32_t(std::uint16_t(top))<<16U);
            word[2]=std::uint32_t(std::uint16_t(right))|(std::uint32_t(std::uint16_t(bottom))<<16U);
            word[3]=std::uint32_t(u);word[4]=std::uint32_t(v);
            word[5]=std::uint32_t(du);word[6]=std::uint32_t(dv);
            ++packed_count_;
            return;
        }
        RasterCommand command;
        command.left=left;command.top=top;command.right=right;command.bottom=bottom;
        command.even=even;command.odd=odd;command.dither=dither;command.tag=tag;
        command.u=u;command.v=v;command.du=du;command.dv=dv;command.reserved1=8U;
        if(pack_) pack_whole(command);
        else commands.push_back(command);
    }
    std::uint32_t texture(std::span<const std::uint8_t> pixels) {
        // A textured polygon asks once per scanline for the same texture,
        // and a frame uses a handful of them: the last answer, then a short
        // list, then the map. A frame within the list allocates no map node.
        if(pixels.data()==last_texture_) return last_texture_offset_;
        last_texture_=pixels.data();
        for(std::size_t i=0;i<recent_texture_count_;++i)
            if(recent_textures_[i].first==pixels.data())
                return last_texture_offset_=recent_textures_[i].second;
        if(recent_texture_count_==recent_textures_.size()) {
            const auto found=textures_.find(pixels.data());
            if(found!=textures_.end()) return last_texture_offset_=found->second;
        }
        const auto offset=std::uint32_t(texels.size());
        texels.insert(texels.end(),pixels.begin(),pixels.end());
        if(recent_texture_count_<recent_textures_.size())
            recent_textures_[recent_texture_count_++]={pixels.data(),offset};
        else textures_.emplace(pixels.data(),offset);
        return last_texture_offset_=offset;
    }
    // Packed recording, for a consumer that reads packed_words() itself (the
    // 3DS scene compiler). A world frame is hundreds to thousands of
    // commands; at 96 bytes each the stream alone outgrows a 16 KB data
    // cache several times over, to be written once and read back once.
    // Packed, a plain rectangle takes 12 bytes and an edge run 28; anything
    // else is kept whole. `commands` stays empty until unpack(), which
    // rebuilds exactly the commands an unpacked recorder would hold.
    // Survives reset().
    void set_packed(bool enabled) noexcept {pack_=enabled;}
    [[nodiscard]] bool packed() const noexcept {return pack_;}
    [[nodiscard]] std::size_t command_count() const noexcept {
        return pack_?packed_count_:commands.size();
    }
    [[nodiscard]] std::span<const std::uint32_t> packed_words() const noexcept {
        return {packed_.data(),packed_size_};
    }
    // Word 0 of a packed record: bits 0-1 kind, 2 dither, 3 span surface,
    // 4-7 tag, 8-15 even, 16-23 odd, 24-31 dither scale (reserved0, plain
    // rectangles only). Words 1-2: left|top<<16, right|bottom<<16 as signed
    // 16-bit halves. A run adds u, v, du, dv; a whole command follows its
    // kind word as 24 words.
    static constexpr std::uint32_t packed_solid=1U,packed_run=2U,packed_whole=3U;
    static constexpr std::size_t packed_solid_words=3U,packed_run_words=7U,
        packed_whole_words=1U+sizeof(RasterCommand)/sizeof(std::uint32_t);
    // Fills in `commands` from the packed stream. A no-op for an unpacked
    // recorder, or when nothing was recorded since the last call.
    void unpack() {
        if(!pack_ || unpacked_words_==packed_size_+1U) return;
        commands.clear();commands.reserve(packed_count_);
        for(std::size_t at=0;at<packed_size_;) {
            const auto* word=packed_.data()+at;
            const auto kind=word[0]&3U;
            RasterCommand command;
            if(kind==packed_whole) {
                std::memcpy(static_cast<void*>(&command),word+1,sizeof(command));
                at+=packed_whole_words;
            } else {
                command.dither=(word[0]>>2U)&1U;command.tag=(word[0]>>4U)&15U;
                command.even=(word[0]>>8U)&255U;command.odd=(word[0]>>16U)&255U;
                command.left=std::int16_t(word[1]);command.top=std::int16_t(word[1]>>16U);
                command.right=std::int16_t(word[2]);command.bottom=std::int16_t(word[2]>>16U);
                if(kind==packed_run) {
                    command.u=std::int32_t(word[3]);command.v=std::int32_t(word[4]);
                    command.du=std::int32_t(word[5]);command.dv=std::int32_t(word[6]);
                    command.reserved1=8U;
                    at+=packed_run_words;
                } else {
                    command.reserved0=word[0]>>24U;
                    if(word[0]&8U) command.surface[2]=1.0F;
                    at+=packed_solid_words;
                }
            }
            commands.push_back(command);
        }
        unpacked_words_=packed_size_+1U;
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
    // A consumer that draws slanted sides itself takes one edge run in place
    // of one solid span per row. Survives reset().
    void accept_edge_runs(bool accept) noexcept {edge_runs_=accept;}
    bool accepts_edge_runs() const noexcept {return edge_runs_;}
    std::vector<RasterCommand> commands;
    std::vector<std::uint8_t> texels;
    std::vector<std::uint32_t> rows,indices;
private:
    [[nodiscard]] bool rejects(std::int32_t left,std::int32_t top,
        std::int32_t right,std::int32_t bottom) const noexcept {
        return right<=0 || bottom<=0 || left>=int(width_) || top>=int(height_)
            || left>=right || top>=bottom;
    }
    // Whether these fields fit a packed record's bit fields.
    [[nodiscard]] static bool fits_packed(std::int32_t left,std::int32_t top,std::int32_t right,
        std::int32_t bottom,std::uint32_t even,std::uint32_t odd,std::uint32_t dither,
        std::uint32_t tag) noexcept {
        // Four signed 16-bit coordinates, two bytes, one bit, one nibble.
        const auto high=[](std::int32_t value) {return (std::uint32_t(value)+32768U)>>16U;};
        return (high(left)|high(top)|high(right)|high(bottom)
            |((even|odd)>>8U)|(dither>>1U)|(tag>>4U))==0U;
    }
    // Room for `count` more packed words. The vector's size is the room
    // reserved so far; packed_size_ is what a frame has used of it.
    [[nodiscard]] std::uint32_t* packed_room(std::size_t count) {
        if(packed_size_+count>packed_.size())
            packed_.resize(std::max(packed_.size()*2U,packed_size_+count+1024U));
        auto* const word=packed_.data()+packed_size_;
        packed_size_+=count;
        return word;
    }
    // add() of an arbitrary command to the packed stream: as a plain
    // rectangle or an edge run when it is exactly one, whole otherwise.
    void pack_command(const RasterCommand& command) {
        const bool plain=command.textured==0U && command.has_surface==0U
            && command.texture_offset==0U && command.u_mask==0U && command.v_mask==0U
            && command.colour_base==0U
            && fits_packed(command.left,command.top,command.right,command.bottom,
                command.even,command.odd,command.dither,command.tag);
        static constexpr std::array<float,4> none{},span{0.0F,0.0F,1.0F,0.0F};
        const auto same_bits=[&](const std::array<float,4>& surface) {
            return std::memcmp(command.surface.data(),surface.data(),sizeof(surface))==0;
        };
        if(plain && command.reserved1==0U && command.reserved0<=255U
            && command.u==0 && command.v==0 && command.du==0 && command.dv==0
            && (same_bits(none) || same_bits(span))) {
            add_solid(command.left,command.top,command.right,command.bottom,command.even,
                command.odd,command.dither,command.tag,same_bits(span),command.reserved0);
            return;
        }
        if(plain && command.reserved1==8U && command.reserved0==0U && same_bits(none)) {
            add_edge_run(command.left,command.top,command.right,command.bottom,command.even,
                command.odd,command.dither,command.tag,command.u,command.v,command.du,command.dv);
            return;
        }
        pack_whole(command);
    }
    void pack_whole(const RasterCommand& command) {
        auto* const word=packed_room(packed_whole_words);
        word[0]=packed_whole;
        std::memcpy(word+1,&command,sizeof(command));
        ++packed_count_;
    }
    std::uint32_t width_{},height_{};
    bool edge_runs_{};
    std::vector<std::uint32_t> cursors_;
    std::unordered_map<const std::uint8_t*,std::uint32_t> textures_;
    bool pack_{};
    std::vector<std::uint32_t> packed_;
    std::size_t packed_size_{},packed_count_{};
    // packed_size_+1 at the last unpack(); zero when `commands` is stale.
    std::size_t unpacked_words_{};
    const std::uint8_t* last_texture_{};
    std::uint32_t last_texture_offset_{};
    std::array<std::pair<const std::uint8_t*,std::uint32_t>,8> recent_textures_{};
    std::size_t recent_texture_count_{};
};
}
