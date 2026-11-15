#pragma once
#include "wdc65816_fast.hpp"
#include "cpu/65816/cpu_65c816.h"
#include <array>
#include <cstdint>
#include <algorithm>
#if defined(__GNUC__) || defined(__clang__)
#define STARFOX_FAST_INLINE inline __attribute__((always_inline))
#define STARFOX_FAST_COLD __attribute__((noinline, cold))
#define STARFOX_FAST_FLATTEN __attribute__((flatten))
#define STARFOX_FAST_LIKELY(condition) __builtin_expect(!!(condition), 1)
#define STARFOX_FAST_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#define STARFOX_FAST_UNREACHABLE() __builtin_unreachable()
#else
#define STARFOX_FAST_INLINE inline
#define STARFOX_FAST_COLD
#define STARFOX_FAST_FLATTEN
#define STARFOX_FAST_LIKELY(condition) (condition)
#define STARFOX_FAST_UNLIKELY(condition) (condition)
#define STARFOX_FAST_UNREACHABLE() return false
#endif



namespace starfox::simulation::detail {

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
// The register file the library's opcode templates operate on. Member names,
// accessor bodies and access order mirror WDC65C816 in cpu_65c816.h; only
// ReadU8/WriteU8 differ, and cycles are counted from entry in 32 bits.
struct Fast65816Core {
    struct Reg {
        union {
            std::uint16_t u16;
            std::uint8_t u8[2];
        };
        void get(std::uint8_t& v) const { v = u8[0]; }
        void get(std::uint16_t& v) const { v = u16; }
        void set(std::uint8_t v) { u8[0] = v; }
        void set(std::uint16_t v) { u16 = v; }
    };
    struct Registers {
        union {
            struct {
                Reg a, x, y, d, sp;
            };
            Reg reglist[5];
        };
    };
    struct State {
        Registers regs;
        std::uint32_t data_segment_base;
        std::uint32_t cycle;
        std::uint32_t code_segment_base;
        std::uint32_t ip;
        std::uint32_t zero, negative, interrupts, carry, other_flags;
    };
    enum InterruptType { BRK, COP, IRQ, NMI };
    enum Attention : std::uint32_t {
        // An access left plain memory: the bus ran, and with it a device.
        left_plain_memory = 1U,
        // The caller services transfer requests between instructions.
        serviced = 2U,
    };
    static constexpr std::uint32_t internal_cycle_timing = 1U;

    State cpu_state;
    bool mode_native_6502;
    bool mode_emulation;
    bool mode_long_a;
    bool mode_long_xy;
    bool supports_decimal;
    bool fast_block_moves;

    // Dispatch row: the library's native mode number, times 256.
    std::uint32_t mode;
    std::uint8_t open_bus;
    // Why the loop must look up after this instruction (Attention bits).
    std::uint32_t attention;
    std::size_t executed;
    const Fast65816Tables* tables;
    ::WDC65C816* real;
    SystemBus* bus;
    std::uint64_t base_cycle;
    std::uint64_t base_instructions;
#ifdef STARFOX_VERIFY_FAST_65816
    bool declined;
    std::uint32_t write_count;
    std::array<std::uint32_t, 16> write_address;
    std::array<std::uint8_t, 16> write_value;
#endif

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
        mode = ((mode_long_a ? 1U : 0U) + (mode_long_xy ? 2U : 0U)) << 8U;
        open_bus = bus->open_bus;
    }
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
    STARFOX_FAST_INLINE void attach(::WDC65C816& cpu, const Fast65816Tables& source) {
        real = &cpu;
        bus = cpu.sys;
        tables = &source;
        base_instructions = cpu.num_emulated_instructions;
        executed = 0U;
        attention = 0U;
        mode_native_6502 = false;
        mode_emulation = false;
        supports_decimal = cpu.supports_decimal;
        fast_block_moves = cpu.fast_block_moves;
#ifdef STARFOX_VERIFY_FAST_65816
        declined = false;
        write_count = 0U;
#endif
        load();
    }
    STARFOX_FAST_INLINE void fetched(std::uint8_t opcode) {
        cpu_state.cycle += 1U;
        open_bus = opcode;
    }

#ifdef STARFOX_VERIFY_FAST_65816
    void ReadU8(std::uint32_t addr, std::uint8_t& v) {
        const std::uint32_t masked = addr & 0xffffffU;
        cpu_state.cycle += 1U;
        for (std::uint32_t index = write_count; index != 0U; --index) {
            if (write_address[index - 1U] != masked) continue;
            v = write_value[index - 1U];
            open_bus = v;
            return;
        }
        const std::uint8_t* page = tables->read[masked >> Fast65816Tables::page_bits];
        if (page == nullptr) {
            declined = true;
            v = 0U;
            return;
        }
        v = page[masked & 0xfffU];
        open_bus = v;
    }
    void WriteU8(std::uint32_t addr, std::uint8_t v) {
        const std::uint32_t masked = addr & 0xffffffU;
        cpu_state.cycle += 1U;
        if (tables->write[masked >> Fast65816Tables::page_bits] == nullptr
            || write_count == write_address.size()) {
            declined = true;
            return;
        }
        write_address[write_count] = masked;
        write_value[write_count++] = v;
        open_bus = v;
    }
    // Held-back stores must be seen by later reads: go byte by byte then.
    const std::uint8_t* readable_pair(std::uint32_t masked) const {
        const std::uint8_t* page = tables->read[masked >> Fast65816Tables::page_bits];
        return write_count != 0U || page == nullptr ? nullptr : page + (masked & 0xfffU);
    }
    std::uint8_t* writable_pair(std::uint32_t) const { return nullptr; }
#else
    // The library's bus. Its devices see memory and the bus itself, never
    // the CPU: the registers stay here, and reach the library's CPU when a
    // device throws (see lean_execute). Neither function takes this
    // object, so the compiler may keep all of it in registers.
    // Returns the byte read, and above it the open bus byte left behind.
    STARFOX_FAST_COLD static std::uint32_t leave_to_read(SystemBus& bus,
        std::uint32_t addr, std::uint8_t open_bus_before) {
        bus.open_bus = open_bus_before;
        std::uint8_t value;
        bus.ReadByte(addr, &value);
        return value | (std::uint32_t{bus.open_bus} << 8U);
    }
    // Returns the open bus byte left behind.
    STARFOX_FAST_COLD static std::uint8_t leave_to_write(SystemBus& bus,
        std::uint32_t addr, std::uint8_t v, std::uint8_t open_bus_before) {
        bus.open_bus = open_bus_before;
        bus.WriteByte(addr, v);
        return bus.open_bus;
    }
    STARFOX_FAST_INLINE void ReadU8(std::uint32_t addr, std::uint8_t& v) {
        const std::uint32_t masked = addr & 0xffffffU;
        const std::uint8_t* page = tables->read[masked >> Fast65816Tables::page_bits];
        if (STARFOX_FAST_LIKELY(page != nullptr)) {
            v = page[masked & 0xfffU];
            open_bus = v;
        } else {
            const std::uint32_t result = leave_to_read(*bus, addr, open_bus);
            v = static_cast<std::uint8_t>(result);
            open_bus = static_cast<std::uint8_t>(result >> 8U);
            attention |= left_plain_memory;
        }
        cpu_state.cycle += 1U;
    }
    STARFOX_FAST_INLINE void WriteU8(std::uint32_t addr, std::uint8_t v) {
        const std::uint32_t masked = addr & 0xffffffU;
        std::uint8_t* page = tables->write[masked >> Fast65816Tables::page_bits];
        if (STARFOX_FAST_LIKELY(page != nullptr)) {
            page[masked & 0xfffU] = v;
            open_bus = v;
        } else {
            open_bus = leave_to_write(*bus, addr, v, open_bus);
            attention |= left_plain_memory;
        }
        cpu_state.cycle += 1U;
    }
    STARFOX_FAST_INLINE const std::uint8_t* readable_pair(std::uint32_t masked) const {
        const std::uint8_t* page = tables->read[masked >> Fast65816Tables::page_bits];
        return page == nullptr ? nullptr : page + (masked & 0xfffU);
    }
    STARFOX_FAST_INLINE std::uint8_t* writable_pair(std::uint32_t masked) const {
        std::uint8_t* page = tables->write[masked >> Fast65816Tables::page_bits];
        return page == nullptr ? nullptr : page + (masked & 0xfffU);
    }
#endif

    // Two accesses, low byte first, as the library makes them. When
    // `second` follows `first` in one plain page they share its lookup.
    STARFOX_FAST_INLINE void read_pair(std::uint32_t first, std::uint32_t second,
        std::uint16_t& v) {
        const std::uint32_t masked = first & 0xffffffU;
        if (STARFOX_FAST_LIKELY((masked & 0xfffU) != 0xfffU)) {
            const std::uint8_t* at = readable_pair(masked);
            if (STARFOX_FAST_LIKELY(at != nullptr)) {
                const std::uint8_t high = at[1];
                v = static_cast<std::uint16_t>(at[0] | (high << 8));
                open_bus = high;
                cpu_state.cycle += 2U;
                return;
            }
        }
        std::uint8_t low, high;
        ReadU8(first, low);
        ReadU8(second, high);
        v = low | ((std::uint16_t)high << 8);
    }
    STARFOX_FAST_INLINE void write_pair(std::uint32_t first, std::uint32_t second,
        std::uint16_t v) {
        const std::uint32_t masked = first & 0xffffffU;
        if (STARFOX_FAST_LIKELY((masked & 0xfffU) != 0xfffU)) {
            std::uint8_t* at = writable_pair(masked);
            if (STARFOX_FAST_LIKELY(at != nullptr)) {
                at[0] = (std::uint8_t)v;
                at[1] = (std::uint8_t)(v >> 8);
                open_bus = (std::uint8_t)(v >> 8);
                cpu_state.cycle += 2U;
                return;
            }
        }
        WriteU8(first, (std::uint8_t)v);
        WriteU8(second, (std::uint8_t)(v >> 8));
    }

    // Everything from here on belongs inside each opcode. GCC's flatten on
    // the loop puts it there; Clang wants it said.
    STARFOX_FAST_INLINE void InternalOp(std::uint32_t n) { cpu_state.cycle += internal_cycle_timing * n; }
    STARFOX_FAST_INLINE void InternalOp() { cpu_state.cycle += internal_cycle_timing; }

    // Disassembly helpers of the opcode templates; never called here.
    STARFOX_FAST_INLINE void PeekU8(std::uint32_t, std::uint8_t& v) { v = 0U; }
    STARFOX_FAST_INLINE std::uint8_t PeekU8(std::uint32_t) { return 0U; }
    STARFOX_FAST_INLINE std::uint16_t PeekU16(std::uint32_t) { return 0U; }

    STARFOX_FAST_INLINE void ReadU16NoCrossBank(std::uint32_t base, std::uint32_t addr, std::uint16_t& v) {
        read_pair(base | (addr & 0xFFFF), base | ((addr + 1) & 0xFFFF), v);
    }
    STARFOX_FAST_INLINE void ReadPBR(std::uint32_t addr, std::uint8_t& v) {
        ReadU8(cpu_state.code_segment_base | (addr & 0xFFFF), v);
    }
    STARFOX_FAST_INLINE void ReadPBR(std::uint32_t addr, std::uint16_t& v) {
        ReadRaw(cpu_state.code_segment_base | (addr & 0xFFFF), v);
    }
    STARFOX_FAST_INLINE void ReadDBR(std::uint32_t addr, std::uint8_t& v) {
        ReadU8((addr & 0xFFFF) | cpu_state.data_segment_base, v);
    }
    STARFOX_FAST_INLINE void ReadDBR(std::uint32_t addr, std::uint16_t& v) {
        ReadU16NoCrossBank(cpu_state.data_segment_base, addr, v);
    }
    STARFOX_FAST_INLINE void WriteDBR(std::uint32_t addr, std::uint8_t v) {
        WriteU8((addr & 0xFFFF) | cpu_state.data_segment_base, v);
    }
    STARFOX_FAST_INLINE void WriteDBR(std::uint32_t addr, std::uint16_t v) {
        write_pair((addr & 0xFFFF) | cpu_state.data_segment_base,
            ((addr + 1) & 0xFFFF) | cpu_state.data_segment_base, v);
    }
    STARFOX_FAST_INLINE void ReadZero(std::uint32_t addr, std::uint8_t& v) { ReadU8(addr & 0xFFFF, v); }
    STARFOX_FAST_INLINE void ReadZero(std::uint32_t addr, std::uint16_t& v) { ReadU16NoCrossBank(0, addr, v); }
    STARFOX_FAST_INLINE void WriteZero(std::uint32_t addr, std::uint8_t v) { WriteU8(addr & 0xFFFF, v); }
    STARFOX_FAST_INLINE void WriteZero(std::uint32_t addr, std::uint16_t v) {
        write_pair(addr & 0xFFFF, (addr + 1) & 0xFFFF, v);
    }
    STARFOX_FAST_INLINE void ReadRaw(std::uint32_t addr, std::uint8_t& v) { ReadU8(addr, v); }
    STARFOX_FAST_INLINE void ReadRaw(std::uint32_t addr, std::uint16_t& v) { read_pair(addr, addr + 1, v); }
    STARFOX_FAST_INLINE void WriteRaw(std::uint32_t addr, std::uint8_t v) { WriteU8(addr, v); }
    STARFOX_FAST_INLINE void WriteRaw(std::uint32_t addr, std::uint16_t v) { write_pair(addr, addr + 1, v); }

    STARFOX_FAST_INLINE void Push(std::uint8_t v) {
        WriteZero(cpu_state.regs.sp.u16, v);
        if (mode_emulation)
            cpu_state.regs.sp.u8[0]--;
        else
            cpu_state.regs.sp.u16--;
    }
    STARFOX_FAST_INLINE void Push(std::uint16_t v) {
        // High byte at S, low byte at S-1: one lookup when the page holds both.
        const std::uint32_t top = cpu_state.regs.sp.u16;
        if (STARFOX_FAST_LIKELY(!mode_emulation && (top & 0xfffU) != 0U)) {
            std::uint8_t* at = writable_pair(top - 1U);
            if (STARFOX_FAST_LIKELY(at != nullptr)) {
                at[1] = (std::uint8_t)(v >> 8);
                at[0] = (std::uint8_t)(v & 0xFF);
                open_bus = (std::uint8_t)(v & 0xFF);
                cpu_state.cycle += 2U;
                cpu_state.regs.sp.u16 -= 2;
                return;
            }
        }
        Push((std::uint8_t)(v >> 8));
        Push((std::uint8_t)(v & 0xFF));
    }
    STARFOX_FAST_INLINE void PopOld(std::uint8_t& v) {
        if (mode_emulation)
            cpu_state.regs.sp.u8[0]++;
        else
            cpu_state.regs.sp.u16++;
        ReadZero(cpu_state.regs.sp.u16, v);
    }
    STARFOX_FAST_INLINE void PopOld(std::uint16_t& v) { Pop(v); }
    STARFOX_FAST_INLINE void Pop(std::uint8_t& v) {
        ReadZero(cpu_state.regs.sp.u16 + 1, v);
        if (mode_emulation)
            cpu_state.regs.sp.u8[0]++;
        else
            cpu_state.regs.sp.u16++;
    }
    STARFOX_FAST_INLINE void Pop(std::uint16_t& v) {
        // Low byte at S+1, high byte at S+2.
        const std::uint32_t first = (cpu_state.regs.sp.u16 + 1U) & 0xFFFFU;
        if (STARFOX_FAST_LIKELY(!mode_emulation && (first & 0xfffU) != 0xfffU)) {
            const std::uint8_t* at = readable_pair(first);
            if (STARFOX_FAST_LIKELY(at != nullptr)) {
                const std::uint8_t high = at[1];
                v = static_cast<std::uint16_t>(at[0] | (high << 8));
                open_bus = high;
                cpu_state.cycle += 2U;
                cpu_state.regs.sp.u16 += 2;
                return;
            }
        }
        std::uint8_t hi, lo;
        Pop(lo);
        Pop(hi);
        v = ((std::uint16_t)hi << 8) | lo;
    }

    STARFOX_FAST_INLINE void FastBlockMove(std::uint32_t src_bank, std::uint32_t dest_bank, std::uint16_t increment) {
        src_bank <<= 16;
        dest_bank <<= 16;
        while (cpu_state.regs.a.u16 != 0xFFFF) {
            std::uint32_t src = src_bank | cpu_state.regs.x.u16;
            std::uint32_t dst = dest_bank | cpu_state.regs.y.u16;
            cpu_state.regs.x.u16 += increment;
            cpu_state.regs.y.u16 += increment;
            cpu_state.regs.a.u16--;

            std::uint8_t byte;
            ReadU8(src, byte);
            WriteU8(dst, byte);
        }
        cpu_state.data_segment_base = dest_bank;
    }
    STARFOX_FAST_INLINE bool BlockMove(std::uint32_t src_bank, std::uint32_t dest_bank, std::uint16_t increment) {
        if (cpu_state.regs.a.u16 == 0xFFFF)
            return true;
        if (fast_block_moves) {
            FastBlockMove(src_bank, dest_bank, increment);
            return true;
        }
        src_bank <<= 16;
        dest_bank <<= 16;
        std::uint32_t src = src_bank | cpu_state.regs.x.u16;
        std::uint32_t dst = dest_bank | cpu_state.regs.y.u16;
        cpu_state.regs.x.u16 += increment;
        cpu_state.regs.y.u16 += increment;
        cpu_state.regs.a.u16--;

        std::uint8_t byte;
        ReadU8(src, byte);
        WriteU8(dst, byte);
        cpu_state.data_segment_base = dest_bank;

        return false;
    }

    // Native mode only: XCE is left to the library.
    STARFOX_FAST_INLINE void OnUpdateMode() {
        mode = ((mode_long_a ? 1U : 0U) + (mode_long_xy ? 2U : 0U)) << 8U;
        if (!mode_long_xy) {
            cpu_state.regs.x.u8[1] = 0;
            cpu_state.regs.y.u8[1] = 0;
        }
    }

    STARFOX_FAST_INLINE void SetNZ(std::uint8_t v) {
        cpu_state.zero = v;
        cpu_state.negative = v >> 7;
    }
    STARFOX_FAST_INLINE void SetNZ(std::uint16_t v) {
        cpu_state.zero = v;
        cpu_state.negative = v >> 15;
    }
    STARFOX_FAST_INLINE std::uint8_t GetStatusRegister() {
        std::uint8_t v = cpu_state.other_flags & 0x48;
        if (!cpu_state.zero)
            v |= 2;
        if (!cpu_state.interrupts)
            v |= 4;
        if (cpu_state.carry & 2)
            v |= 1;
        if (cpu_state.negative & 1)
            v |= 0x80;
        if (mode_emulation) {
            v |= 0x30;
        } else {
            if (!mode_long_a)
                v |= 0x20;
            if (!mode_long_xy)
                v |= 0x10;
        }
        return v;
    }
    STARFOX_FAST_INLINE void SetStatusRegister(std::uint8_t v) {
        cpu_state.other_flags = v;
        cpu_state.zero = 2 - (v & 2);
        cpu_state.carry = (v & 1) << 1;
        cpu_state.interrupts = 1 - ((v >> 2) & 1);
        cpu_state.negative = v >> 7;

        bool changed_mode = false;
        if (mode_long_a != !(v & 0x20)) {
            changed_mode = true;
            mode_long_a = !(v & 0x20);
        }
        if (mode_long_xy != !(v & 0x10)) {
            changed_mode = true;
            mode_long_xy = !(v & 0x10);
        }
        if (changed_mode)
            OnUpdateMode();
    }

    // Reached only through opcodes that `leaves_to_library` excludes.
    void WAI() {}
    void STP() {}
    void DoInterrupt(InterruptType) {}
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace starfox::simulation::detail
