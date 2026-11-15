// Native equivalents of the hottest loops of the cartridge logic.
// See wdc65816_kernels.hpp for the contract.
//
// Three loops, each entered at its head by the compact interpreter: the pair
// loop of the collision pass (CHKCOLLNXT, CHKCOLL0, NORMALCOL), the first loop
// of INIT_STRATS_RAM_L, and RESET_SPRITES. A kernel first checks the
// loop's bytes where they run, the register widths and the bytes it would
// store; when anything differs it returns false, leaving the machine as it
// was, and the interpreter runs the loop. Otherwise it runs whole iterations
// and stops in front of the first one it cannot prove, leaving work RAM,
// registers, raw flag words, open bus, cycle and instruction counts exactly
// as the interpreter would leave them at that instruction boundary.
//
// Every iteration below is the original instruction sequence written out:
// each line names the instructions it stands for and adds their count and
// their cycles as the compact interpreter charges them (the pinned library's
// figures, not the processor manual's). The verification build
// (STARFOX_VERIFY_KERNELS) runs the interpreter over the same instructions
// and compares all of it.
#include "wdc65816_kernels.hpp"
#include "wdc65816_watch.hpp"

#include <algorithm>
#include <cstring>

namespace starfox::simulation::detail {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

inline u32 rd16(const u8* memory, u32 at) noexcept {
    return u32{memory[at]} | (u32{memory[at + 1U]} << 8U);
}
inline void wr16(u8* memory, u32 at, u32 value) noexcept {
    memory[at] = static_cast<u8>(value);
    memory[at + 1U] = static_cast<u8>(value >> 8U);
}
// An indexed absolute read costs one more cycle when the index carries into
// the high byte of the base.
inline u32 cross(u32 base, u32 index) noexcept {
    return (((base + index) ^ base) & 0xff00U) != 0U ? 1U : 0U;
}
// An indexed direct-page access costs one more when offset plus index
// passes $ff.
inline u32 wide(u32 index, u32 offset) noexcept {
    return index + offset > 0xffU ? 1U : 0U;
}
inline bool touches(const std::array<u32, 2>& watch, u32 at, u32 size) noexcept {
    return watch[0] - at < size || watch[1] - at < size;
}

// The stack of a cartridge call sits in page one. Objects are read as words up
// to offset $2f, through the direct page and through bank $7e alike, so they
// must lie above the stack and inside the 8 KB both views share.
constexpr u32 object_fields = 0x30U;
inline bool object_at(u32 pointer, u32 limit) noexcept {
    return pointer >= 0x0200U && pointer + object_fields <= limit;
}

// Reads a routine out of work RAM, instruction by instruction.
struct Reader {
    const u8* memory;
    u32 at;
    bool ok{true};

    u8 byte() noexcept { const u8 value = memory[at & 0xffffU]; ++at; return value; }
    u32 word() noexcept { const u32 low = byte(); return low | (u32{byte()} << 8U); }
    void op(u8 opcode) noexcept { if (byte() != opcode) ok = false; }
    void op(u8 opcode, u8 operand) noexcept { op(opcode); if (byte() != operand) ok = false; }
    u32 absolute(u8 opcode) noexcept { op(opcode); return word(); }
    void absolute(u8 opcode, u32 value) noexcept { if (absolute(opcode) != value) ok = false; }
    u32 direct(u8 opcode) noexcept { op(opcode); return byte(); }
    void direct(u8 opcode, u32 value) noexcept { if (direct(opcode) != value) ok = false; }
    void branch(u8 opcode, u32 target) noexcept {
        op(opcode);
        const u32 distance = byte();
        if (((at + (distance ^ 0x80U) - 0x80U) & 0xffffU) != (target & 0xffffU)) ok = false;
    }
    // A three-byte jump, BRL or JMP absolute (the two cartridges differ);
    // both cost the same. Returns its last byte, which stays on the bus.
    u8 jump(u32 target) noexcept {
        const u8 opcode = byte();
        const u32 operand = word();
        if (opcode == 0x4cU) { if (operand != (target & 0xffffU)) ok = false; }
        else if (opcode == 0x82U) { if (((at + operand) & 0xffffU) != (target & 0xffffU)) ok = false; }
        else ok = false;
        return static_cast<u8>(operand >> 8U);
    }
};

struct Axes {
    std::array<u32, 3> field, half, range, position, centre;
    std::array<u8, 3> bus;
};

// Three copies of: LDA field,X / CLC / ADC half / STA range / LDA position /
// SEC / SBC centre / BPL +4 / EOR #$ffff / INC A / SEC / SBC range / BMI next /
// JMP reject. The third BMI goes to `hit`.
void read_axes(Reader& r, Axes& axes, u32 reject, u32 hit) noexcept {
    for (unsigned axis = 0U; axis < 3U; ++axis) {
        const u32 start = r.at;
        axes.field[axis] = r.absolute(0xbdU);
        r.op(0x18U);
        axes.half[axis] = r.absolute(0x6dU);
        axes.range[axis] = r.absolute(0x8dU);
        axes.position[axis] = r.direct(0xa5U);
        r.op(0x38U);
        axes.centre[axis] = r.direct(0xe5U);
        r.branch(0x10U, r.at + 6U);
        r.absolute(0x49U, 0xffffU);
        r.op(0x1aU);
        r.op(0x38U);
        r.absolute(0xedU, axes.range[axis]);
        r.branch(0x30U, axis == 2U ? hit : start + 30U);
        axes.bus[axis] = r.jump(reject);
    }
}

} // namespace

bool LeanKernels::Pairs::capture(const u8* wram, u32 head_, u32 filter_, u32 axes_) {
    valid = false;
    head = head_; filter = filter_; axes = axes_;
    for (const u32 start : {head, filter, axes})
        if (start < 0x2000U || start > 0xfe00U) return false;

    // CHKCOLL0: the tests that need no arithmetic.
    Reader r{wram, filter};
    r.op(0xe2U, 0x20U);
    index = r.absolute(0x8cU);
    list = r.absolute(0xbeU);
    hit = r.absolute(0x9cU);
    r.op(0xc2U, 0x20U);
    current = r.direct(0xa4U);
    r.absolute(0xb9U, 0x001dU); r.absolute(0x29U, 0x0080U);
    r.branch(0xf0U, r.at + 5U); filter_bus[0] = r.jump(head);
    r.absolute(0xb9U, 0x002eU); r.direct(0x35U, 0x2eU); r.absolute(0x29U, 0x00f8U);
    r.branch(0xf0U, r.at + 5U); filter_bus[1] = r.jump(head);
    r.op(0x98U); r.direct(0xd5U, 0x19U);
    r.branch(0xd0U, r.at + 5U); filter_bus[2] = r.jump(head);
    r.op(0x8aU); r.op(0xbbU); r.direct(0xd5U, 0x19U);
    r.branch(0xd0U, r.at + 5U); static_cast<void>(r.jump(head));
    r.op(0x9bU); r.op(0xaaU);
    r.direct(0xb5U, 0x0cU); stored[0] = r.direct(0x85U);
    r.direct(0xb5U, 0x0eU); stored[1] = r.direct(0x85U);
    r.direct(0xb5U, 0x10U); stored[2] = r.direct(0x85U);
    r.direct(0xb5U, 0x1fU); r.absolute(0x29U, 0x0080U); r.branch(0xf0U, r.at + 15U);
    r.absolute(0xb9U, 0x001fU); r.absolute(0x29U, 0x0080U); r.branch(0xf0U, r.at + 7U);
    r.direct(0xb5U, 0x04U); r.op(0x9bU); r.branch(0x80U, r.at + 13U);
    r.direct(0xb5U, 0x04U); r.op(0x9bU); shape = r.absolute(0xcdU);
    r.branch(0xd0U, r.at + 5U); filter_bus[3] = r.jump(head);
    r.absolute(0xaeU, index);
    r.absolute(0xbdU, list + 2U);
    r.branch(0xd0U, r.at + 5U); static_cast<void>(r.jump(axes));
    // The entry has a box list: same three tests, between a push and a pull.
    r.op(0x48U);
    for (unsigned n = 0U; n < 3U; ++n) {
        r.direct(0xa5U, stored[n]);
        copied[n] = r.absolute(0x8dU);
    }
    Axes boxed{};
    const u32 leave = r.at + 90U;
    read_axes(r, boxed, leave, leave + 4U);
    r.op(0x68U);
    box_bus = r.jump(head);
    const u32 filter_end = r.at;

    // NORMALCOL: the three tests for an entry without boxes.
    Reader n{wram, axes};
    Axes plain{};
    read_axes(n, plain, head, axes + 90U);

    // CHKCOLLNXT: next entry of the list.
    Reader h{wram, head};
    h.op(0xc2U, 0x20U);
    h.absolute(0xadU, index);
    h.op(0x18U);
    h.absolute(0x69U, 0x000aU);
    h.op(0xa8U);
    count = h.absolute(0xceU);
    h.branch(0xf0U, h.at + 5U);
    static_cast<void>(h.jump(filter));
    if (!r.ok || !n.ok || !h.ok) return false;

    range = plain.range[0];
    for (unsigned axis = 0U; axis < 3U; ++axis) {
        if (plain.field[axis] != boxed.field[axis] || plain.half[axis] != boxed.half[axis]
            || plain.position[axis] != boxed.position[axis] || plain.centre[axis] != boxed.centre[axis]
            || plain.range[axis] != range || boxed.range[axis] != range) return false;
        field[axis] = plain.field[axis];
        half[axis] = plain.half[axis];
        centre[axis] = plain.centre[axis];
        axis_bus[axis] = plain.bus[axis];
        const auto found = std::find(stored.begin(), stored.end(), plain.position[axis]);
        if (found == stored.end()) return false;
        position[axis] = static_cast<u32>(found - stored.begin());
        // The list fields are the entry's other words.
        if (field[axis] < list + 4U || field[axis] > list + 8U) return false;
    }

    // Variables: absolute ones above the object table, inside the 8 KB that
    // bank zero mirrors; nothing the loop stores may overlap anything else.
    struct Variable { u32 at, size; bool stored; };
    const std::array variables{
        Variable{index, 2U, true}, Variable{count, 2U, true}, Variable{hit, 1U, true},
        Variable{range, 2U, true}, Variable{copied[0], 2U, true}, Variable{copied[1], 2U, true},
        Variable{copied[2], 2U, true}, Variable{stored[0], 2U, true}, Variable{stored[1], 2U, true},
        Variable{stored[2], 2U, true}, Variable{current, 2U, false}, Variable{shape, 2U, false},
        Variable{half[0], 2U, false}, Variable{half[1], 2U, false}, Variable{half[2], 2U, false},
        Variable{centre[0], 2U, false}, Variable{centre[1], 2U, false}, Variable{centre[2], 2U, false}};
    low = 0x2000U; high = 0U;
    for (std::size_t a = 0U; a < variables.size(); ++a) {
        const auto& one = variables[a];
        if (one.at < 0x0002U || one.at + one.size > 0x2000U) return false;
        // Direct-page variables stay below the stack page.
        if (one.at + one.size > 0x0100U && one.at < 0x0200U) return false;
        if (one.at >= 0x0200U) { low = std::min(low, one.at); high = std::max(high, one.at + one.size); }
        for (std::size_t b = 0U; b < variables.size(); ++b) {
            const auto& other = variables[b];
            if (a == b || (!one.stored && !other.stored)) continue;
            if (one.at < other.at + other.size && other.at < one.at + one.size) return false;
        }
    }
    if (low < 0x0200U + object_fields) return false;
    // The list sits above everything stored and below the code.
    if (list < 0x2000U || list > 0x3000U) return false;

    spans = {Span{filter, filter_end - filter}, Span{axes, 90U}, Span{head, h.at - head}};
    std::size_t used = 0U;
    for (const auto& span : spans) {
        if (used + span.size > bytes.size()) return false;
        std::memcpy(bytes.data() + used, wram + span.at, span.size);
        used += span.size;
    }
    valid = true;
    return true;
}

bool LeanKernels::Pairs::unchanged(const u8* wram) const noexcept {
    std::size_t used = 0U;
    for (const auto& span : spans) {
        if (std::memcmp(bytes.data() + used, wram + span.at, span.size) != 0) return false;
        used += span.size;
    }
    return true;
}

// CHKCOLLNXT .. CHKCOLL0 .. NORMALCOL: one entry of the collision list against
// the current object. Runs the entries that end in "no collision" and stops
// before a hit, a path no console replay has taken, or the end of the list.
// It stores to its own variables and to the stack only, never to an object:
// nothing the watched block's mirror looks at.
// Declines outside bank $7e, when the three routines do not have the shapes
// Pairs::capture() reads, without 16-bit X/Y, D = 0, DBR = $7e and binary
// mode, when a variable it stores to holds a serviced byte, or when the
// current object does not lie between the stack page and those variables.
// The last entry, a box-list entry whose word cannot be pushed, and any entry
// begun with fewer than 128 instructions of budget are left to the
// interpreter.
bool LeanKernels::run_pairs(u32 address, u8* w, const std::array<u32, 2>& watch,
    LeanKernelState& k) {
    Pairs& p = pairs_;
    if ((address >> 16U) != 0x7eU || pairs_filter == 0U || pairs_axes == 0U) return false;
    if ((!p.valid || p.head != (address & 0xffffU) || !p.unchanged(w))
        && !p.capture(w, address & 0xffffU, pairs_filter, pairs_axes)) return false;
    // 16-bit index registers, direct page at zero, data bank $7e, binary mode.
    if (k.long_xy == 0U || k.d != 0U || k.dbr != 0x7e0000U || (k.other & 8U) != 0U) return false;
    if (touches(watch, p.index, 2U) || touches(watch, p.count, 2U) || touches(watch, p.hit, 1U)
        || touches(watch, p.range, 2U)) return false;
    for (unsigned n = 0U; n < 3U; ++n)
        if (touches(watch, p.copied[n], 2U) || touches(watch, p.stored[n], 2U)) return false;
    // The box list pushes one word.
    const bool stack_usable = k.s >= 0x0101U && k.s <= 0x01ffU && !touches(watch, k.s - 1U, 2U);
    const u32 current = rd16(w, p.current);
    if (!object_at(current, p.low)) return false;

    u32 index = rd16(w, p.index), count = rd16(w, p.count);
    u32 cycles = 0U;
    std::size_t instructions = 0U;
    u32 a = 0U, x = 0U, y = 0U, zero = 0U, negative = 0U, carry = 0U, overflow = 0U, bus = 0U;
    for (;;) {
        // The last entry (and a count that would wrap) is the interpreter's.
        if (count < 2U || k.remaining - instructions < 128U) break;
        u32 cy = 0U, n = 0U;
        // CHKCOLLNXT: REP #$20 / LDA index / CLC / ADC #$000a / TAY / DEC count / BEQ / JMP
        const u32 sum = index + 10U;
        const u32 entry = sum & 0xffffU;
        if (entry > 0x1000U) break;
        const u32 sum_carry = sum >> 15U;
        const u32 sum_overflow = ((index ^ sum) & ~(index ^ 10U) & 0x8000U) != 0U ? 0x40U : 0U;
        cy += 2U + 5U + 2U + 3U + 2U + 8U + 2U + 3U; n += 8U;
        // CHKCOLL0: SEP #$20 / STY index / LDX list,Y / STZ hit / REP #$20 / LDY current
        const u32 other = rd16(w, p.list + entry);
        if (!object_at(other, p.low)) break;
        cy += 2U + 5U + 5U + cross(p.list, entry) + 4U + 2U + 4U; n += 6U;

        bool positions = false, tested = false, boxed = false;
        u32 px = 0U, py = 0U, pz = 0U, range = 0U, box = 0U;
        // LDA $001d,Y / AND #$0080 / BEQ
        u32 m = rd16(w, current + 0x1dU) & 0x0080U;
        cy += 5U + cross(0x001dU, current) + 3U; n += 2U;
        if (m != 0U) {                                    // the current object is already hit
            cy += 2U + 3U; n += 2U;                       // BEQ falls through / jump
            a = m; x = other; y = current; zero = m; negative = 0U;
            carry = sum_carry; overflow = sum_overflow; bus = p.filter_bus[0];
        } else {
            cy += 3U; n += 1U;
            // LDA $002e,Y / AND $2e,X / AND #$00f8 / BEQ
            m = rd16(w, current + 0x2eU) & rd16(w, other + 0x2eU) & 0x00f8U;
            cy += 5U + cross(0x002eU, current) + 5U + wide(other, 0x2eU) + 3U; n += 3U;
            if (m != 0U) {                                // same side
                cy += 2U + 3U; n += 2U;
                a = m; x = other; y = current; zero = m; negative = 0U;
                carry = sum_carry; overflow = sum_overflow; bus = p.filter_bus[1];
            } else {
                cy += 3U; n += 1U;
                // TYA / CMP $19,X / BNE
                cy += 2U + 5U + wide(other, 0x19U); n += 2U;
                if (rd16(w, other + 0x19U) == current) {  // the other one is immune to this one
                    cy += 2U + 3U; n += 2U;
                    a = current; x = other; y = current; zero = 0U; negative = 0U;
                    carry = 2U; overflow = sum_overflow; bus = p.filter_bus[2];
                } else {
                    cy += 3U; n += 1U;
                    // TXA / TYX / CMP $19,X / BNE
                    cy += 2U + 2U + 5U + wide(current, 0x19U); n += 3U;
                    if (rd16(w, current + 0x19U) == other) break;   // never seen on a console
                    cy += 3U; n += 1U;
                    // TXY / TAX / LDA $0c,X / STA / LDA $0e,X / STA / LDA $10,X / STA
                    px = rd16(w, other + 0x0cU); py = rd16(w, other + 0x0eU); pz = rd16(w, other + 0x10U);
                    positions = true;
                    cy += 2U + 2U + 3U * (5U + 4U) + wide(other, 0x0cU) + wide(other, 0x0eU)
                        + wide(other, 0x10U); n += 8U;
                    // LDA $1f,X / AND #$0080 / BEQ
                    cy += 5U + wide(other, 0x1fU) + 3U; n += 2U;
                    if ((rd16(w, other + 0x1fU) & 0x0080U) != 0U) break;   // never seen on a console
                    cy += 3U; n += 1U;
                    // LDA $04,X / TXY / CMP shape / BNE
                    const u32 shape = rd16(w, other + 0x04U);
                    cy += 5U + wide(other, 0x04U) + 2U + 5U; n += 3U;
                    if (shape == rd16(w, p.shape)) {      // same shape as the current box
                        cy += 2U + 3U; n += 2U;
                        a = shape; x = other; y = other; zero = 0U; negative = 0U;
                        carry = 2U; overflow = sum_overflow; bus = p.filter_bus[3];
                    } else {
                        cy += 3U; n += 1U;
                        // LDX index / LDA list+2,X / BNE
                        box = rd16(w, p.list + 2U + entry);
                        cy += 5U + 5U + cross(p.list + 2U, entry); n += 2U;
                        if (box != 0U) {
                            if (!stack_usable) break;
                            boxed = true;
                            // BNE taken / PHA / LDA,STA x3
                            cy += 3U + 4U + 3U * (4U + 5U); n += 8U;
                        } else {
                            cy += 2U + 3U; n += 2U;       // BNE falls through / jump to NORMALCOL
                        }
                        const std::array<u32, 3> place{px, py, pz};
                        u32 result = 0U, magnitude = 0U;
                        int rejected = -1;
                        for (unsigned axis = 0U; axis < 3U; ++axis) {
                            // LDA field,X / CLC / ADC half / STA range
                            const u32 base = p.field[axis];
                            range = (rd16(w, base + entry) + rd16(w, p.half[axis])) & 0xffffU;
                            cy += 5U + cross(base, entry) + 2U + 5U + 5U; n += 4U;
                            // LDA position / SEC / SBC centre / BPL (/ EOR #$ffff / INC A)
                            const u32 difference = place[p.position[axis]] - rd16(w, p.centre[axis]);
                            magnitude = difference & 0xffffU;
                            cy += 4U + 2U + 4U; n += 3U;
                            if ((difference >> 15U) & 1U) {
                                magnitude = (0U - magnitude) & 0xffffU;
                                cy += 2U + 3U + 2U; n += 3U;
                            } else {
                                cy += 3U; n += 1U;
                            }
                            // SEC / SBC range / BMI
                            result = magnitude - range;
                            cy += 2U + 5U; n += 2U;
                            if (((result >> 15U) & 1U) == 0U) { rejected = static_cast<int>(axis); break; }
                            cy += 3U; n += 1U;
                        }
                        if (rejected < 0) break;          // inside on all three axes: a hit
                        tested = true;
                        cy += 2U + 3U; n += 2U;           // BMI falls through / JMP
                        x = entry; y = other;
                        carry = ~(result >> 15U);
                        overflow = ((magnitude ^ result) & (magnitude ^ range) & 0x8000U) != 0U
                            ? 0x40U : 0U;
                        if (boxed) {
                            cy += 5U + 3U; n += 2U;       // PLA / JMP
                            a = box; zero = box; negative = box >> 15U; bus = p.box_bus;
                        } else {
                            a = result & 0xffffU; zero = result; negative = result >> 15U;
                            bus = p.axis_bus[static_cast<unsigned>(rejected)];
                        }
                    }
                }
            }
        }
        // The entry ended in "no collision": what it left in memory.
        wr16(w, p.index, entry);
        wr16(w, p.count, count - 1U);
        w[p.hit] = 0U;
        if (positions) {
            wr16(w, p.stored[0], px); wr16(w, p.stored[1], py); wr16(w, p.stored[2], pz);
        }
        if (tested) wr16(w, p.range, range);
        if (boxed) {
            wr16(w, p.copied[0], px); wr16(w, p.copied[1], py); wr16(w, p.copied[2], pz);
            w[k.s] = static_cast<u8>(box >> 8U);
            w[k.s - 1U] = static_cast<u8>(box);
        }
        index = entry; --count;
        cycles += cy; instructions += n;
    }
    if (instructions == 0U) return false;
    k.a = a; k.x = x; k.y = y;
    k.zero = zero; k.negative = negative; k.carry = carry;
    // REP #$20 cleared the width bit; the last ADC or SBC left overflow.
    k.other = ((k.other & ~0x20U) & 0xffbfU) | overflow;
    k.long_a = 1U;
    k.open_bus = bus;
    k.cycle += cycles;
    k.remaining -= instructions;
    return true;
}

// INIT_STRATS_RAM_L, first loop: clears the collision marks of every object
// in the list. Runs every object but the last.
// Declines unless the 42 bytes at the head, in bank $7e, are this loop, with an
// 8-bit accumulator, 16-bit X/Y and D = 0. Stops before an object outside
// $0200..$1fd0 or one whose bytes $1b..$2d hold a serviced byte, and when
// fewer than 32 instructions of budget remain. Each object's bytes $1b..$2d
// are marked in the watched block before they change.
bool LeanKernels::run_objects(u32 address, const u8* code, u32 room, u8* w,
    const std::array<u32, 2>& watch, LeanWatch& marks, LeanKernelState& k) {
    if ((address >> 16U) != 0x7eU || room < 42U) return false;
    const u32 marked = (address + 16U) & 0xffffU;
    const u8 expected[42] = {
        0xb5, 0x1d,                   // LDA $1d,X
        0x29, 0x80,                   // AND #$80
        0xf0, 0x04,                   // BEQ +4
        0x5c, static_cast<u8>(marked), static_cast<u8>(marked >> 8U), 0x7e,   // JML marked
        0xa9, 0x01,                   // LDA #$01
        0x95, 0x2d,                   // STA $2d,X
        0x80, 0x06,                   // BRA +6
        0xb5, 0x1e,                   // marked: LDA $1e,X
        0x09, 0x02,                   // ORA #$02
        0x95, 0x1e,                   // STA $1e,X
        0xb5, 0x1d,                   // LDA $1d,X
        0x29, 0x7f,                   // AND #$7f
        0x95, 0x1d,                   // STA $1d,X
        0xc2, 0x20,                   // REP #$20
        0xa9, 0x00, 0x00,             // LDA #$0000
        0x95, 0x1b,                   // STA $1b,X
        0xe2, 0x20,                   // SEP #$20
        0x9b,                         // TXY
        0xb6, 0x00,                   // LDX $00,Y
        0xd0, 0xd6};                  // BNE head
    if (std::memcmp(code, expected, sizeof expected) != 0) return false;
    // 8-bit accumulator, 16-bit index registers, direct page at zero.
    if (k.long_a != 0U || k.long_xy == 0U || k.d != 0U) return false;

    u32 x = k.x, y = k.y, cycles = 0U;
    std::size_t instructions = 0U;
    for (;;) {
        if (!object_at(x, 0x2000U) || touches(watch, x + 0x1bU, 0x13U)) break;
        const u32 next = rd16(w, x);
        // The last object is the interpreter's.
        if (next == 0U || k.remaining - instructions < 32U) break;
        // Bytes $1b to $2d of the object are about to change.
        marks.store(w + x + 0x1bU, 0x13U);
        const u32 flags = w[x + 0x1dU];
        u32 cy = 4U + wide(x, 0x1dU) + 2U, n = 2U;       // LDA $1d,X / AND #$80
        if ((flags & 0x80U) != 0U) {
            // BEQ falls through / JML / LDA $1e,X / ORA #$02 / STA $1e,X
            w[x + 0x1eU] = static_cast<u8>(w[x + 0x1eU] | 0x02U);
            cy += 2U + 4U + 4U + wide(x, 0x1eU) + 2U + 4U + wide(x, 0x1eU); n += 5U;
        } else {
            // BEQ taken / LDA #$01 / STA $2d,X / BRA
            w[x + 0x2dU] = 0x01U;
            cy += 3U + 2U + 4U + wide(x, 0x2dU) + 3U; n += 4U;
        }
        // LDA $1d,X / AND #$7f / STA $1d,X
        w[x + 0x1dU] = static_cast<u8>(flags & 0x7fU);
        cy += 4U + wide(x, 0x1dU) + 2U + 4U + wide(x, 0x1dU); n += 3U;
        // REP #$20 / LDA #$0000 / STA $1b,X / SEP #$20
        wr16(w, x + 0x1bU, 0U);
        cy += 2U + 3U + 5U + wide(x, 0x1bU) + 2U; n += 4U;
        // TXY / LDX $00,Y / BNE taken
        cy += 2U + 5U + wide(x, 0x00U) + 3U; n += 3U;
        y = x; x = next;
        cycles += cy; instructions += n;
    }
    if (instructions == 0U) return false;
    k.a = 0U; k.x = x; k.y = y;
    k.zero = x; k.negative = x >> 15U;
    k.other |= 0x20U;
    k.open_bus = 0xd6U;
    k.cycle += cycles;
    k.remaining -= instructions;
    return true;
}

// RESET_SPRITES: STZ base,X / INX / INX / DEC A / DEC A / BPL. Clears the
// rest of the sprite block, a word at a time. Runs every turn but the last.
// The block lies between the two object tables.
// Declines unless these nine bytes (with any base) are at the head, A and X/Y
// are 16-bit, the data bank shows low work RAM, A is at most $7fff, 16
// instructions of budget remain and at least one turn fits, or when the
// cleared range leaves $0200..$1fff or holds a serviced byte. The range is
// marked before it is cleared.
bool LeanKernels::run_sprites(const u8* code, u32 room, const std::array<u32, 2>& watch,
    u8* w, LeanWatch& marks, LeanKernelState& k) {
    if (room < 9U || code[0] != 0x9eU || code[3] != 0xe8U || code[4] != 0xe8U
        || code[5] != 0x3aU || code[6] != 0x3aU || code[7] != 0x10U || code[8] != 0xf7U) return false;
    // 16-bit registers; the data bank must show the low work RAM.
    if (k.long_a == 0U || k.long_xy == 0U || (k.dbr != 0U && k.dbr != 0x7e0000U)) return false;
    if (k.a > 0x7fffU || k.remaining < 16U) return false;
    // Turns that end with the branch taken.
    const u32 turns = std::min<u32>(k.a >> 1U, static_cast<u32>(
        std::min<std::size_t>((k.remaining - 8U) / 6U, 0x4000U)));
    if (turns == 0U) return false;
    const u32 first = (u32{code[1]} | (u32{code[2]} << 8U)) + k.x, size = turns * 2U;
    if (first < 0x0200U || first + size > 0x2000U || touches(watch, first, size)) return false;
    marks.store(w + first, size);
    std::memset(w + first, 0, size);
    k.a -= size; k.x += size;
    k.zero = k.a; k.negative = k.a >> 15U;
    k.open_bus = 0xf7U;
    k.cycle += turns * 16U;                                // 5 + 2 + 2 + 2 + 2 + 3
    k.remaining -= turns * std::size_t{6U};
    return true;
}

bool LeanKernels::run(Kind kind, u32 address, const u8* code, u32 room, u8* wram,
    const std::array<u32, 2>& watch, LeanWatch& marks, LeanKernelState& state) {
    if (wram == nullptr || code == nullptr) return false;
    switch (kind) {
    case pairs: return run_pairs(address, wram, watch, state);
    case objects: return run_objects(address, code, room, wram, watch, marks, state);
    case sprites: return run_sprites(code, room, watch, wram, marks, state);
    default: return false;
    }
}

} // namespace starfox::simulation::detail
