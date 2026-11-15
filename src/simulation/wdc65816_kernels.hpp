#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace starfox::simulation::detail {

struct LeanWatch;

// The compact interpreter's registers and raw flag words between two
// instructions.
struct LeanKernelState {
    std::uint32_t a, x, y, s, d, dbr;
    std::uint32_t zero, negative, carry, other;
    std::uint32_t long_a, long_xy;
    std::uint32_t cycle, open_bus;
    std::size_t remaining;
};

// Native equivalents of the three loops where the cartridge logic spends most
// of its instructions (measured on console replays): the pair loop of the
// collision pass, the per-object reset before it, and the sprite block clear.
//
// A kernel is entered at the head of its loop. It runs whole iterations only,
// and only the ones it can prove take a path it knows; it stops before any
// other, leaving every register, flag word, byte of memory, cycle count and
// instruction count exactly as the interpreter would have at that same
// instruction boundary. The iteration it stopped at is then interpreted.
//
// The loops are recognized by their bytes, read from where they run, so a
// cartridge that differs keeps the interpreter.
class LeanKernels {
public:
    enum Kind : unsigned { pairs, objects, sprites, kinds };

    // `code` is the host address of the loop head, `room` the bytes of plain
    // memory that follow it; `wram` is $7e0000. `serviced` holds the offsets
    // in work RAM of the bytes a store may not touch, or ~0. Stores into the
    // watched block leave their marks in `marks`.
    bool run(Kind kind, std::uint32_t address, const std::uint8_t* code, std::uint32_t room,
        std::uint8_t* wram, const std::array<std::uint32_t, 2>& serviced, LeanWatch& marks,
        LeanKernelState& state);

    // Where the collision pass keeps its loop, for the pair kernel:
    // CHKCOLL0 and NORMALCOL (16-bit addresses in bank $7e).
    std::uint32_t pairs_filter{}, pairs_axes{};

private:
    struct Pairs {
        bool valid{};
        std::uint32_t head{}, filter{}, axes{};
        // Variables: absolute addresses in bank $7e, or direct-page offsets.
        std::uint32_t list{}, index{}, count{}, hit{}, current{}, shape{}, range{};
        std::array<std::uint32_t, 3> stored{};     // direct page: the other object's X, Y, Z
        std::array<std::uint32_t, 3> copied{};     // absolute: where the box list copies them
        // The three axis tests, in program order.
        std::array<std::uint32_t, 3> field{};      // absolute,X: half size in the list entry
        std::array<std::uint32_t, 3> half{};       // absolute: half size of the current object
        std::array<std::uint32_t, 3> position{};   // index into `stored`
        std::array<std::uint32_t, 3> centre{};     // direct page: the current object's centre
        // Open bus left by the jump that ends each way out of a pair.
        std::array<std::uint8_t, 4> filter_bus{};
        std::array<std::uint8_t, 3> axis_bus{};
        std::uint8_t box_bus{};
        std::uint32_t low{}, high{};               // extent of the variables
        struct Span { std::uint32_t at, size; };
        std::array<Span, 3> spans{};
        std::array<std::uint8_t, 512> bytes{};
        bool capture(const std::uint8_t* wram, std::uint32_t head, std::uint32_t filter, std::uint32_t axes);
        [[nodiscard]] bool unchanged(const std::uint8_t* wram) const noexcept;
    };
    Pairs pairs_;
    bool run_pairs(std::uint32_t address, std::uint8_t* wram,
        const std::array<std::uint32_t, 2>& watch, LeanKernelState& state);
    static bool run_objects(std::uint32_t address, const std::uint8_t* code, std::uint32_t room,
        std::uint8_t* wram, const std::array<std::uint32_t, 2>& watch, LeanWatch& marks,
        LeanKernelState& state);
    static bool run_sprites(const std::uint8_t* code, std::uint32_t room,
        const std::array<std::uint32_t, 2>& watch, std::uint8_t* wram, LeanWatch& marks,
        LeanKernelState& state);
};

} // namespace starfox::simulation::detail
