#pragma once
#include <array>
#include <cstdint>

namespace starfox::render {
// A source frame owns one starting endpoint, shared by every interpolated
// presentation/eye. Only its first presentation may advance the carried state.
class GridLineHistory {
public:
    struct Frame {
        std::uint64_t number{};
        std::array<std::int16_t,2> start{};
        bool advances{};
    };
    Frame begin(std::uint64_t number) noexcept {
        const bool fresh=!initialized_ || number_!=number;
        if(fresh) {
            initialized_=true;number_=number;start_=previous_;committed_=false;
        }
        return {number,start_,fresh};
    }
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
