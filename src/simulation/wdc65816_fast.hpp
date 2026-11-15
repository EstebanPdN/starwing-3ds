#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "wdc65816_kernels.hpp"
#include "wdc65816_watch.hpp"

class WDC65C816;
struct SystemBus;

// Interface between Wdc65816's call and task loops (wdc65816.cpp) and the
// compact interpreter (wdc65816_lean.cpp): page and stop tables, per-call
// parameters, and the hooks of the verification builds.

namespace starfox::simulation::detail {

// A second dispatch loop for the pinned RetroCPU 65C816. Its handlers,
// generated from the library's opcode table, reproduce the library's
// instructions access by access, so registers, the raw flag words, cycles,
// open bus and every executed PC stay identical; only the bus lookup and the
// per-instruction bookkeeping are specialized. Anything it does not cover is
// left, untouched, to the library's interpreter.
struct Fast65816Tables {
    static constexpr std::uint32_t page_bits = 12U;
    static constexpr std::size_t page_count = std::size_t{1} << (24U - page_bits);
    // Why the caller acts on a program address between two instructions.
    enum Stop : std::uint8_t {
        stop_always = 1U,     // crash handlers, the zero projection loop
        stop_transfer = 4U,   // blocks the transfer calls run natively
        stop_return = 8U,     // $01f0, where a call's return sentinel may be
        stop_kernel = 16U,    // the head of a loop with a native equivalent
    };
    struct Range { std::uint32_t first, last; std::uint8_t stop; };
    static constexpr std::uint8_t never_runs = 0xffU;

    // Pages that are plain memory for every address they hold; null for
    // I/O, unmapped and (write only) read-only pages.
    std::array<std::uint8_t*, page_count> read{};
    std::array<std::uint8_t*, page_count> write{};
    // The same, minus each page's own address: zero for null.
    std::array<std::uintptr_t, page_count> read_biased{};
    std::array<std::uintptr_t, page_count> write_biased{};
    mutable LeanWatch watch;
    // Per page: 0 when no address in it stops the loop, `never_runs` when
    // none may execute here, else one plus its row in `stop_pages`.
    std::array<std::uint8_t, page_count> guarded{};
    // The Stop bits of each address of a page that holds a stop.
    std::vector<std::array<std::uint8_t, std::size_t{1} << page_bits>> stop_pages;
    // The marked offsets of each row of `stop_pages`, ascending.
    std::vector<std::vector<std::uint16_t>> stop_marks;
    // Call after adding a row to `stop_pages` or marking an address in one.
    void refresh_stop_marks(std::size_t row) {
        if (stop_marks.size() <= row) stop_marks.resize(row + 1U);
        auto& marks = stop_marks[row];
        marks.clear();
        for (std::uint32_t offset = 0U; offset < stop_pages[row].size(); ++offset)
            if (stop_pages[row][offset] != 0U) marks.push_back(static_cast<std::uint16_t>(offset));
    }
    // Loop heads marked stop_kernel, by LeanKernels::Kind; zero when absent.
    std::array<std::uint32_t, LeanKernels::kinds> kernel_entries{};
    mutable LeanKernels kernels;
    bool usable{};
    // $7e0000 when all of work RAM is one plain block that bank zero mirrors
    // in its first 8 KB; null otherwise.
    const std::uint8_t* collision_ram{};
    // Diagnostic only, deliberately outside serialized machine state: runs
    // and instructions of the object and sprite kernels, then of the pair one.
    mutable std::array<std::uint64_t,4> native_counts{};
    // Compiled calls/instructions, interpreted instructions, missing entries,
    // rejected code bytes, and short budgets. Not part of saved game state.
    mutable std::array<std::uint64_t,6> dispatch_counts{};

    // Builds the tables from the bus pages and the stop ranges. A zero first
    // address is an absent symbol. `usable` stays false, so every instruction
    // goes to the library, when the bus does not have the layout the loop
    // assumes.
    void build(const SystemBus& bus, std::uint32_t rom_banks,
        std::span<const Range> stop_ranges);
};

// One run of the loop on behalf of a call or task.
struct Fast65816Call {
    // The program address at which the call has returned.
    std::uint32_t return_sentinel{};
    // Fast65816Tables::Stop bits this call acts on.
    std::uint8_t stops{};
    // The caller's ring of the last 32 program counters and its position.
    std::uint32_t* recent{};
    std::size_t instructions{};
    // The most instructions this run may execute.
    std::size_t budget{};
    // Bytes the caller services between instructions when they are non-zero.
    const std::uint8_t* service[2]{};
    // Verification builds only: what the loop does at a kernel entry.
    enum Kernels : std::uint8_t {
        kernels_run,          // run it and go on (the only mode of a product build)
        kernels_stop,         // return in front of it
        kernels_after_first,  // interpret this instruction, return in front of the next entry
        kernels_only,         // run it and return
        kernels_off,          // interpret
    };
    Kernels kernels{kernels_run};
};

// Executes instructions while each one could have been dispatched by the
// caller's loop with no other effect, and returns how many ran. Zero is always
// a valid answer. This is the compact interpreter of wdc65816_lean.cpp.
std::size_t run_lean_65816(WDC65C816& cpu, const Fast65816Tables& tables,
    const Fast65816Call& call);

#ifdef STARFOX_VERIFY_KERNELS
// Verification build: every kernel run is repeated by the interpreter from the
// same state and compared, registers, flag words, cycles and all of work RAM.
// A difference throws std::logic_error.
struct LeanKernelVerifyStats {
    std::array<std::uint64_t, LeanKernels::kinds> compared, instructions, declined;
};
LeanKernelVerifyStats lean_kernel_verify_stats() noexcept;
#endif

#ifdef STARFOX_VERIFY_FAST_65816
// Verification build: nothing is executed here. One instruction is run on a
// copy of the registers with its stores held back, for comparison with the
// library's interpreter running the same instruction for real.
struct Fast65816Probe {
    std::uint16_t a, x, y, d, sp;
    std::uint32_t data_segment_base, code_segment_base, ip;
    std::uint32_t zero, negative, interrupts, carry, other_flags, cycles;
    std::uint8_t open_bus;
    bool long_a, long_xy;
    std::uint32_t pc;
    std::uint8_t opcode;
    std::uint32_t write_count;
    std::array<std::uint32_t, 16> write_address;
    std::array<std::uint8_t, 16> write_value;
};
bool probe_fast_65816(WDC65C816& cpu, const Fast65816Tables& tables,
    const Fast65816Call& call, Fast65816Probe& probe);
void begin_fast_65816_reference_step() noexcept;
// Throws std::logic_error naming the first difference.
void check_fast_65816(const Fast65816Probe& probe, const WDC65C816& cpu,
    std::uint64_t cycle_before);
struct Fast65816VerifyStats { std::uint64_t compared, skipped; };
Fast65816VerifyStats fast_65816_verify_stats() noexcept;
#endif

} // namespace starfox::simulation::detail
