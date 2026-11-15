#pragma once
#include "wdc65816_fast.hpp"
#include "cpu/65816/cpu_65c816.h"
#include <cstddef>
#include <cstdint>
#if defined(__GNUC__) || defined(__clang__)
#define STARFOX_FAST_INLINE inline __attribute__((always_inline))
#define STARFOX_FAST_COLD __attribute__((noinline, cold))
#define STARFOX_FAST_LIKELY(condition) __builtin_expect(!!(condition), 1)
#define STARFOX_FAST_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#define STARFOX_FAST_INLINE inline
#define STARFOX_FAST_COLD
#define STARFOX_FAST_LIKELY(condition) (condition)
#define STARFOX_FAST_UNLIKELY(condition) (condition)
#endif

// The compact interpreter's (wdc65816_lean.cpp) local copy of the library's CPU
// state, and the inlining and branch-hint macros it is compiled with.

namespace starfox::simulation::detail {

// The CPU state the compact interpreter borrows from the library's WDC65C816
// for one run and gives back at the end. Field names follow cpu_65c816.h;
// cycles are counted from entry in 32 bits.
struct Fast65816Core {
    struct Reg {
        std::uint16_t u16;
    };
    struct Registers {
        Reg a, x, y, d, sp;
    };
    struct State {
        Registers regs;
        std::uint32_t data_segment_base;
        std::uint32_t cycle;
        std::uint32_t code_segment_base;
        std::uint32_t ip;
        std::uint32_t zero, negative, interrupts, carry, other_flags;
    };

    State cpu_state;
    bool mode_long_a;
    bool mode_long_xy;
    std::uint8_t open_bus;
    // Instructions run since attach().
    std::size_t executed;
    ::WDC65C816* real;
    SystemBus* bus;
    std::uint64_t base_cycle;
    std::uint64_t base_instructions;

    STARFOX_FAST_INLINE void load() {
        const auto& source = real->cpu_state;
        cpu_state.regs.a.u16 = source.regs.a.u16;
        cpu_state.regs.x.u16 = source.regs.x.u16;
        cpu_state.regs.y.u16 = source.regs.y.u16;
        cpu_state.regs.d.u16 = source.regs.d.u16;
        cpu_state.regs.sp.u16 = source.regs.sp.u16;
        cpu_state.data_segment_base = source.data_segment_base;
        cpu_state.code_segment_base = source.code_segment_base;
        cpu_state.ip = source.ip;
        cpu_state.zero = source.zero;
        cpu_state.negative = source.negative;
        cpu_state.interrupts = source.interrupts;
        cpu_state.carry = source.carry;
        cpu_state.other_flags = source.other_flags;
        cpu_state.cycle = 0U;
        base_cycle = source.cycle;
        mode_long_a = real->mode_long_a;
        mode_long_xy = real->mode_long_xy;
        open_bus = bus->open_bus;
    }
    // Writes the state back, with the cycles and instructions run here added
    // to the library's counters.
    STARFOX_FAST_INLINE void store() {
        auto& target = real->cpu_state;
        target.regs.a.u16 = cpu_state.regs.a.u16;
        target.regs.x.u16 = cpu_state.regs.x.u16;
        target.regs.y.u16 = cpu_state.regs.y.u16;
        target.regs.d.u16 = cpu_state.regs.d.u16;
        target.regs.sp.u16 = cpu_state.regs.sp.u16;
        target.data_segment_base = cpu_state.data_segment_base;
        target.code_segment_base = cpu_state.code_segment_base;
        target.ip = cpu_state.ip;
        target.zero = cpu_state.zero;
        target.negative = cpu_state.negative;
        target.interrupts = cpu_state.interrupts;
        target.carry = cpu_state.carry;
        target.other_flags = cpu_state.other_flags;
        target.cycle = base_cycle + cpu_state.cycle;
        if (real->mode_long_a != mode_long_a || real->mode_long_xy != mode_long_xy) {
            real->mode_long_a = mode_long_a;
            real->mode_long_xy = mode_long_xy;
            real->OnUpdateMode();
        }
        bus->open_bus = open_bus;
        real->num_emulated_instructions = base_instructions + executed;
    }
    STARFOX_FAST_INLINE void attach(::WDC65C816& cpu) {
        real = &cpu;
        bus = cpu.sys;
        base_instructions = cpu.num_emulated_instructions;
        executed = 0U;
        load();
    }
};

} // namespace starfox::simulation::detail
