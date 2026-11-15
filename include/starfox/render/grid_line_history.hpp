#pragma once
#include <array>
#include <cstdint>

// The start endpoint (the source's M_PREVX/M_PREVY) that DustRenderer's
// connected grid lines carry from one source frame to the next.
namespace starfox::render {
// A source frame owns one starting endpoint, shared by every interpolated
// presentation/eye. Only its first presentation may advance the carried state.
class GridLineHistory {
public:
    // One presentation's view of a source frame: its start endpoint, and
    // whether this presentation may commit the next frame's start.
    struct Frame {
        std::uint64_t number{};
        std::array<std::int16_t,2> start{};
        bool advances{};
    };
    // Joins source frame `number`. A new number starts from the endpoint the
    // previous frame committed, and only that call's Frame advances.
    Frame begin(std::uint64_t number) noexcept {
        const bool fresh=!initialized_ || number_!=number;
        if(fresh) {
            initialized_=true;number_=number;start_=previous_;committed_=false;
        }
        return {number,start_,fresh};
    }
    // Commits `endpoint` as the next source frame's start: once per frame,
    // and only through the Frame that began it.
    void finish(const Frame& frame,std::array<std::int16_t,2> endpoint) noexcept {
        if(initialized_ && frame.number==number_ && frame.advances && !committed_) {
            previous_=endpoint;committed_=true;
        }
    }
    void reset() noexcept { *this=GridLineHistory{}; }
private:
    bool initialized_{},committed_{};
    std::uint64_t number_{};
    std::array<std::int16_t,2> previous_{},start_{};
};
}
