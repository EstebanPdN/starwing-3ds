#pragma once
// Emulator regression builds only (STARWING_AUTOPILOT). Release builds never
// compile this file's behaviour: no SD script is read and no input is added.
//
// sdmc:/3ds/Starwing/autopilot.txt, one step per line, in source ticks:
//   <first> <last> <buttons> [FLOW]  buttons: A B X Y L R S(tart) E(select) U D < >
//                                    FLOW limits the step to one flow label
//   end <tick>                 leave the game loop at that tick
//   gpufail <first> <last>     treat top-screen GPU presentation as failed
//   capture <tick>             save PICA and software images of the same state
//   stereo <strength>          override the slider (0..8)
//   home <tick>                jump to the HOME Menu, as the HOME button does
// Timing rows and events are appended to autopilot.csv / autopilot-events.txt.
#include "starfox/input/buttons.hpp"
#include "starfox/render/framebuffer.hpp"
#include "starfox/render/palette.hpp"
#include "top_viewport.hpp"

#include <3ds.h>
#include <citro3d.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace starfox::platform_3ds {

class Autopilot3ds {
public:
    explicit Autopilot3ds(const char* directory) {
        std::snprintf(directory_,sizeof(directory_),"%s",directory);
        char path[160]{};
        std::snprintf(path, sizeof(path), "%s/autopilot.txt", directory);
        FILE* script = std::fopen(path, "rb");
        if (script == nullptr) return;
        char line[128]{};
        while (std::fgets(line, sizeof(line), script)) {
            unsigned first{}, last{};
            char keys[64]{}, flow[32]{};
            if (std::sscanf(line, "end %u", &first) == 1) {
                end_tick_ = first;
                continue;
            }
            if (std::sscanf(line, "gpufail %u %u", &first, &last) == 2) {
                gpu_failures_.push_back({first, last, 0U, {}});
                continue;
            }
            if (std::sscanf(line, "capture %u", &first) == 1) {
                captures_.push_back(first); continue;
            }
            if (std::sscanf(line, "home %u", &first) == 1) {
                home_tick_ = first; continue;
            }
            if (std::sscanf(line, "stereo %u", &first) == 1) {
                stereo_ = static_cast<int>(first > 8U ? 8U : first); continue;
            }
            const int fields = std::sscanf(line, "%u %u %63s %31s", &first, &last, keys, flow);
            if (fields < 3) continue;
            Step step{first, last, parse(keys), {}};
            if (fields == 4) std::memcpy(step.flow, flow, sizeof(step.flow));
            steps_.push_back(step);
        }
        std::fclose(script);
        active_ = true;
        std::snprintf(path, sizeof(path), "%s/autopilot.csv", directory);
        csv_ = std::fopen(path, "wb");
        std::snprintf(path, sizeof(path), "%s/autopilot-events.txt", directory);
        events_ = std::fopen(path, "wb");
    }
    ~Autopilot3ds() {
        if (csv_) std::fclose(csv_);
        if (events_) std::fclose(events_);
    }
    Autopilot3ds(const Autopilot3ds&) = delete;
    Autopilot3ds& operator=(const Autopilot3ds&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool finished() const noexcept {
        return active_ && end_tick_ != 0U && tick_ >= end_tick_;
    }
    [[nodiscard]] unsigned tick() const noexcept { return tick_; }
    [[nodiscard]] bool force_gpu_failure() const noexcept {
        for (const auto& range : gpu_failures_)
            if (tick_ >= range.first && tick_ <= range.last) return true;
        return false;
    }

    int stereo_override() const noexcept { return stereo_; }
    // True once, at the scripted tick.
    bool take_home_due() noexcept {
        if (home_tick_ == 0U || tick_ < home_tick_) return false;
        home_tick_ = 0U;
        return true;
    }
    bool capture_due() const noexcept {
        return capture_index_ < captures_.size() && tick_ >= captures_[capture_index_];
    }
    void capture(const u8* left, const u8* right) {
        if (!capture_due()) return;
        const auto requested=captures_[capture_index_++];
        for (unsigned side=0;side<2;++side) {
            const auto* pixels=side?right:left;
            if (!pixels) continue;
            GSPGPU_InvalidateDataCache(pixels,400U*240U*2U);
            char path[256]{};
            std::snprintf(path,sizeof(path),"%s/capture-%u-%s.rgb565",directory_,requested,side?"right":"left");
            if (auto* file=std::fopen(path,"wb")) {
                std::fwrite(pixels,2U,400U*240U,file); std::fclose(file);
            }
        }
        event("capture");
    }

    // Explicit readback also resolves the emulator's cached VRAM surfaces.
    void capture_texture(unsigned layer, const C3D_Tex& texture) {
        if (!capture_index_ || !texture.data) return;
        const u32 size=texture.width*texture.height*4U;
        auto* pixels=static_cast<u32*>(linearAlloc(size));
        if(!pixels) return;
        C3D_SyncDisplayTransfer(static_cast<u32*>(texture.data),
            GX_BUFFER_DIM(texture.width,texture.height),pixels,
            GX_BUFFER_DIM(texture.width,texture.height),
            GX_TRANSFER_IN_FORMAT(texture.fmt==GPU_RGBA5551 ? GX_TRANSFER_FMT_RGB5A1 : GX_TRANSFER_FMT_RGBA8)
                |GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8));
        GSPGPU_InvalidateDataCache(pixels,size);
        char path[256]{};
        std::snprintf(path,sizeof(path),"%s/capture-%u-layer-%u.linear",
            directory_,captures_[capture_index_-1U],layer);
        if(auto* f=std::fopen(path,"wb")) {
            const u32 header[]{texture.width,texture.height,0,size};
            std::fwrite(header,sizeof(header),1,f);
            std::fwrite(pixels,size,1,f);std::fclose(f);
        }
        linearFree(pixels);
    }

    // Re-render without PICA from the same simulation/PPU/interpolation state.
    // Store rotated RGB565 just like the real framebuffer; no GPU upload or
    // transfer participates in this reference. Capture runs are not benchmarks.
    void capture_software(const render::Framebuffer& frame,
        const render::Palette256& palette, bool wide, bool right = false) {
        if (!capture_index_) return;
        char path[256]{};
        std::snprintf(path,sizeof(path),"%s/capture-%u-software-%s.rgb565",
            directory_,captures_[capture_index_-1U],right?"right":"left");
        FILE* file=std::fopen(path,"wb");
        if (!file) return;
        const auto view=top_viewport(frame.width(),frame.height(),wide);
        std::uint16_t column[240]{};
        for (unsigned x=0;x<400;++x) {
            for (unsigned y=0;y<240;++y) {
                std::uint16_t value{};
                if (x>=view.x && x<view.x+view.width && y>=view.y && y<view.y+view.height) {
                    const auto c=palette[frame.get(view.sample_x(x),view.sample_y(y))];
                    value=((c.r>>3U)<<11U)|((c.g>>2U)<<5U)|(c.b>>3U);
                }
                column[239U-y]=value;
            }
            std::fwrite(column,sizeof(column),1U,file);
        }
        std::fclose(file);
    }

    // Called once per source tick, before the simulation consumes input.
    void apply(input::TickInput& controls, const char* flow) noexcept {
        if (!active_) return;
        // Script flow names use '_' where the overlay label has a space.
        char name[32]{};
        for (unsigned i = 0U; i + 1U < sizeof(name) && flow[i]; ++i)
            name[i] = flow[i] == ' ' ? '_' : flow[i];
        input::ButtonMask held{};
        for (const auto& step : steps_)
            if (tick_ >= step.first && tick_ <= step.last
                && (step.flow[0] == 0 || std::strcmp(step.flow, name) == 0))
                held |= step.buttons;
        controls.held |= held;
        controls.pressed |= static_cast<input::ButtonMask>(held & ~previous_);
        controls.released |= static_cast<input::ButtonMask>(previous_ & ~held);
        previous_ = held;
        ++tick_;
    }

    void header(const char* columns) noexcept {
        if (!csv_) return;
        std::fprintf(csv_, "tick,top_lit,ppu_mode,%s", columns);
        std::fflush(csv_);
    }
    void row(const char* text, unsigned top_lit, unsigned ppu_mode) noexcept {
        if (!csv_) return;
        std::fprintf(csv_, "%u,%u,%u,%s", tick_, top_lit, ppu_mode, text);
        std::fflush(csv_);
    }
    void event(const char* text) noexcept {
        if (!events_) return;
        std::fprintf(events_, "%llu tick=%u %s\n",
            static_cast<unsigned long long>(osGetTime()), tick_, text);
        std::fflush(events_);
    }

    // Count clearly lit samples on a 50x30 grid of the last presented top
    // image (RGB565, rotated). A black screen reports zero.
    static unsigned lit_samples(const u8* framebuffer) noexcept {
        if (framebuffer == nullptr) return 0U;
        GSPGPU_InvalidateDataCache(framebuffer, 400U * 240U * 2U);
        const auto* pixels = reinterpret_cast<const std::uint16_t*>(framebuffer);
        unsigned lit = 0U;
        for (unsigned x = 4U; x < 400U; x += 8U)
            for (unsigned y = 4U; y < 240U; y += 8U) {
                const auto p = pixels[x * 240U + (239U - y)];
                if (((p >> 11U) & 31U) + ((p >> 5U) & 63U) / 2U + (p & 31U) > 6U) ++lit;
            }
        return lit;
    }

private:
    struct Step { unsigned first, last; input::ButtonMask buttons; char flow[32]; };
    static input::ButtonMask parse(const char* keys) noexcept {
        input::ButtonMask mask{};
        for (const char* c = keys; *c; ++c) {
            switch (*c) {
            case 'A': mask |= input::a; break;
            case 'B': mask |= input::b; break;
            case 'X': mask |= input::x; break;
            case 'Y': mask |= input::y; break;
            case 'L': mask |= input::left_shoulder; break;
            case 'R': mask |= input::right_shoulder; break;
            case 'S': mask |= input::start; break;
            case 'E': mask |= input::select; break;
            case 'U': mask |= input::up; break;
            case 'D': mask |= input::down; break;
            case '<': mask |= input::left; break;
            case '>': mask |= input::right; break;
            default: break;
            }
        }
        return mask;
    }
    char directory_[160]{};
    std::vector<unsigned> captures_;
    std::size_t capture_index_{};
    int stereo_{-1};
    unsigned home_tick_{};
    std::vector<Step> steps_;
    std::vector<Step> gpu_failures_;
    unsigned tick_{}, end_tick_{};
    input::ButtonMask previous_{};
    bool active_{};
    FILE* csv_{};
    FILE* events_{};
};

} // namespace starfox::platform_3ds
