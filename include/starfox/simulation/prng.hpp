#pragma once

#include <array>
#include <cstdint>

// Host model of the retail cartridge's random number generator, used by
// PathVm and host tests. The game itself runs the cartridge's own generator
// on the 65C816 core.

namespace starfox::simulation {

// The pinned build uses rngmode=0. This is a byte-exact translation of the
// chained 65816 SBC sequence in GAME.ASM, including its initial cleared carry.
class OriginalPrng {
public:
    static constexpr std::array<std::uint8_t, 4> kInitialState{
        0x3a, 0xa7, 0x55, 0x7f};

    OriginalPrng() = default;
    explicit OriginalPrng(std::array<std::uint8_t, 4> state) : state_(state) {}

    void reset() noexcept { state_ = kInitialState; }
    // Advances the four state bytes and returns the new state[0].
    [[nodiscard]] std::uint8_t next() noexcept;
    [[nodiscard]] const std::array<std::uint8_t, 4>& state() const noexcept {
        return state_;
    }

private:
    std::array<std::uint8_t, 4> state_{kInitialState};
};

} // namespace starfox::simulation
