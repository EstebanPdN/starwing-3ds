#!/usr/bin/env python3
"""Generates the opcode handlers of the compact 65C816 interpreter.

Every handler reproduces, access by access, what the pinned RetroCPU templates
do in native mode (cpu_65c816_instructions.inl): the same bus accesses in the
same order, the same raw flag words, the same cycle count. Operand bytes are
read through `pc` (first operand byte); the handler macros are defined in
src/simulation/wdc65816_lean.cpp.

The Old 3DS has 16 KB of instruction cache and nothing behind it, so the code
is kept small rather than specialized: an instruction that reaches memory
through an addressing mode runs in two steps that all such instructions share,
one per addressing mode (it leaves the effective address in `ea` and, for a
read, the operand in `m`) and one per operation. The table holds both labels.

usage: generate-lean-65816.py <output.inl>
"""
import sys

# (mnemonic, addressing mode[, extra]) for every opcode, in the library's order.
T = {}
def row(base, *entries):
    for i, e in enumerate(entries):
        T[base + i] = e if isinstance(e, tuple) else (e, None)
X = True   # "check_extra_cycle" of AddrAbsX/AddrAbsY/AddrDirectIndirectY
row(0x00, 'BRK', ('ORA', 'dpindx'), 'COP', ('ORA', 'sr'), ('TSB', 'dp'), ('ORA', 'dp'), ('ASL', 'dp'), ('ORA', 'dpindlong'))
row(0x08, 'PHP', ('ORA', 'imm'), ('ASL', 'acc'), 'PHD', ('TSB', 'abs'), ('ORA', 'abs'), ('ASL', 'abs'), ('ORA', 'long'))
row(0x10, 'BPL', ('ORA', 'dpindy', X), ('ORA', 'dpind'), ('ORA', 'srindy'), ('TRB', 'dp'), ('ORA', 'dpx'), ('ASL', 'dpx'), ('ORA', 'dpindlongy'))
row(0x18, 'CLC', ('ORA', 'absy', X), 'INA', 'TCS', ('TRB', 'abs'), ('ORA', 'absx', X), ('ASL', 'absx', False), ('ORA', 'longx'))
row(0x20, 'JSR', ('AND', 'dpindx'), 'JSL', ('AND', 'sr'), ('BIT', 'dp'), ('AND', 'dp'), ('ROL', 'dp'), ('AND', 'dpindlong'))
row(0x28, 'PLP', ('AND', 'imm'), ('ROL', 'acc'), 'PLD', ('BIT', 'abs'), ('AND', 'abs'), ('ROL', 'abs'), ('AND', 'long'))
row(0x30, 'BMI', ('AND', 'dpindy', X), ('AND', 'dpind'), ('AND', 'srindy'), ('BIT', 'dpx'), ('AND', 'dpx'), ('ROL', 'dpx'), ('AND', 'dpindlongy'))
row(0x38, 'SEC', ('AND', 'absy', X), 'DEA', 'TSC', ('BIT', 'absx', X), ('AND', 'absx', X), ('ROL', 'absx', False), ('AND', 'longx'))
row(0x40, 'RTI', ('EOR', 'dpindx'), 'WDM', ('EOR', 'sr'), 'MVP', ('EOR', 'dp'), ('LSR', 'dp'), ('EOR', 'dpindlong'))
row(0x48, 'PHA', ('EOR', 'imm'), ('LSR', 'acc'), 'PHK', 'JMP', ('EOR', 'abs'), ('LSR', 'abs'), ('EOR', 'long'))
row(0x50, 'BVC', ('EOR', 'dpindy', X), ('EOR', 'dpind'), ('EOR', 'srindy'), 'MVN', ('EOR', 'dpx'), ('LSR', 'dpx'), ('EOR', 'dpindlongy'))
row(0x58, 'CLI', ('EOR', 'absy', X), 'PHY', 'TCD', 'JML', ('EOR', 'absx', X), ('LSR', 'absx', False), ('EOR', 'longx'))
row(0x60, 'RTS', ('ADC', 'dpindx'), 'PER', ('ADC', 'sr'), ('STZ', 'dp'), ('ADC', 'dp'), ('ROR', 'dp'), ('ADC', 'dpindlong'))
row(0x68, 'PLA', ('ADC', 'imm'), ('ROR', 'acc'), 'RTL', 'JMPIND', ('ADC', 'abs'), ('ROR', 'abs'), ('ADC', 'long'))
row(0x70, 'BVS', ('ADC', 'dpindy', X), ('ADC', 'dpind'), ('ADC', 'srindy'), ('STZ', 'dpx'), ('ADC', 'dpx'), ('ROR', 'dpx'), ('ADC', 'dpindlongy'))
row(0x78, 'SEI', ('ADC', 'absy', X), 'PLY', 'TDC', 'JMPINDX', ('ADC', 'absx', X), ('ROR', 'absx', False), ('ADC', 'longx'))
row(0x80, 'BRA', ('STA', 'dpindx'), 'BRL', ('STA', 'sr'), ('STY', 'dp'), ('STA', 'dp'), ('STX', 'dp'), ('STA', 'dpindlong'))
row(0x88, 'DEY', ('BITIMM', 'imm'), 'TXA', 'PHB', ('STY', 'abs'), ('STA', 'abs'), ('STX', 'abs'), ('STA', 'long'))
row(0x90, 'BCC', ('STA', 'dpindy', False), ('STA', 'dpind'), ('STA', 'srindy'), ('STY', 'dpx'), ('STA', 'dpx'), ('STX', 'dpy'), ('STA', 'dpindlongy'))
row(0x98, 'TYA', ('STA', 'absy', False), 'TXS', 'TXY', ('STZ', 'abs'), ('STA', 'absx', False), ('STZ', 'absx', False), ('STA', 'longx'))
row(0xA0, ('LDY', 'imm'), ('LDA', 'dpindx'), ('LDX', 'imm'), ('LDA', 'sr'), ('LDY', 'dp'), ('LDA', 'dp'), ('LDX', 'dp'), ('LDA', 'dpindlong'))
row(0xA8, 'TAY', ('LDA', 'imm'), 'TAX', 'PLB', ('LDY', 'abs'), ('LDA', 'abs'), ('LDX', 'abs'), ('LDA', 'long'))
row(0xB0, 'BCS', ('LDA', 'dpindy', X), ('LDA', 'dpind'), ('LDA', 'srindy'), ('LDY', 'dpx'), ('LDA', 'dpx'), ('LDX', 'dpy'), ('LDA', 'dpindlongy'))
row(0xB8, 'CLV', ('LDA', 'absy', X), 'TSX', 'TYX', ('LDY', 'absx', X), ('LDA', 'absx', X), ('LDX', 'absy', X), ('LDA', 'longx'))
row(0xC0, ('CPY', 'imm'), ('CMP', 'dpindx'), 'REP', ('CMP', 'sr'), ('CPY', 'dp'), ('CMP', 'dp'), ('DEC', 'dp'), ('CMP', 'dpindlong'))
row(0xC8, 'INY', ('CMP', 'imm'), 'DEX', 'WAI', ('CPY', 'abs'), ('CMP', 'abs'), ('DEC', 'abs'), ('CMP', 'long'))
row(0xD0, 'BNE', ('CMP', 'dpindy', X), ('CMP', 'dpind'), ('CMP', 'srindy'), 'PEI', ('CMP', 'dpx'), ('DEC', 'dpx'), ('CMP', 'dpindlongy'))
row(0xD8, 'CLD', ('CMP', 'absy', X), 'PHX', 'STP', 'JMLIND', ('CMP', 'absx', X), ('DEC', 'absx', False), ('CMP', 'longx'))
row(0xE0, ('CPX', 'imm'), ('SBC', 'dpindx'), 'SEP', ('SBC', 'sr'), ('CPX', 'dp'), ('SBC', 'dp'), ('INC', 'dp'), ('SBC', 'dpindlong'))
row(0xE8, 'INX', ('SBC', 'imm'), 'NOP', 'XBA', ('CPX', 'abs'), ('SBC', 'abs'), ('INC', 'abs'), ('SBC', 'long'))
row(0xF0, 'BEQ', ('SBC', 'dpindy', X), ('SBC', 'dpind'), ('SBC', 'srindy'), 'PEA', ('SBC', 'dpx'), ('INC', 'dpx'), ('SBC', 'dpindlongy'))
row(0xF8, 'SED', ('SBC', 'absy', X), 'PLX', 'XCE', 'JSRINDX', ('SBC', 'absx', X), ('INC', 'absx', False), ('SBC', 'longx'))
assert len(T) == 256

A_OPS = {'ORA', 'AND', 'EOR', 'ADC', 'SBC', 'CMP', 'BIT', 'BITIMM', 'LDA', 'STA', 'STZ', 'TSB', 'TRB', 'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'}
XY_OPS = {'LDX', 'LDY', 'STX', 'STY', 'CPX', 'CPY'}

class Mode:
    """Effective-address code of one addressing mode.
    code: statements that leave `ea` (and `obx`, the last bus byte so far, as an expression)
    n: operand bytes; fam: 'RAW' or 'ZERO'; cyc: static cycles spent besides the opcode fetch
    maybe: the library's kMaybeHasConditionalCycle"""
    def __init__(self, n, fam, code, cyc, obx, maybe=False):
        self.n, self.fam, self.code, self.cyc, self.obx, self.maybe = n, fam, code, cyc, obx, maybe

def mode(name, extra):
    w16 = '(pc[0] | (u32(pc[1]) << 8))'
    if name == 'abs':
        return Mode(2, 'RAW', [f'const u32 ea = {w16} + dbr;'], 2, 'pc[1]')
    if name in ('absx', 'absy'):
        reg = 'X' if name == 'absx' else 'Y'
        code = [f'const u32 base = {w16}; const u32 eff = base + {reg};']
        if extra: code.append('if ((eff ^ base) & 0xff00U) ++cycle;')
        code.append('const u32 ea = eff + dbr;')
        return Mode(2, 'RAW', code, 2, 'pc[1]', True)
    if name == 'long':
        return Mode(3, 'RAW', [f'const u32 ea = {w16} | (u32(pc[2]) << 16);'], 3, 'pc[2]')
    if name == 'longx':
        return Mode(3, 'RAW', [f'const u32 ea = ({w16} | (u32(pc[2]) << 16)) + X;'], 3, 'pc[2]')
    if name == 'dp':
        return Mode(1, 'ZERO', ['if (D & 0xffU) ++cycle;', 'const u32 ea = D + pc[0];'], 1, 'pc[0]')
    if name in ('dpx', 'dpy'):
        reg = 'X' if name == 'dpx' else 'Y'
        return Mode(1, 'ZERO', ['if (D & 0xffU) ++cycle;', f'const u32 sum = {reg} + pc[0];', 'if (sum > 0xffU) ++cycle;',
                                'const u32 ea = D + sum;'], 2, 'pc[0]')
    if name == 'sr':
        return Mode(1, 'ZERO', ['const u32 ea = S + pc[0];'], 1, 'pc[0]')
    if name == 'dpind':
        return Mode(1, 'RAW', ['u32 ptr; LEAN_RD16_ZERO(D + pc[0], pc[0], ptr);', 'const u32 ea = dbr + ptr;'], 3, 'ob')
    if name == 'dpindx':
        return Mode(1, 'RAW', ['if (D & 0xffU) ++cycle;', 'const u32 sum = X + pc[0];', 'if (sum > 0xffU) ++cycle;',
                               'u32 ptr; LEAN_RD16_ZERO(D + sum, pc[0], ptr);', 'const u32 ea = dbr + ptr;'], 4, 'ob')
    if name == 'dpindy':
        code = ['if (D & 0xffU) ++cycle;', 'u32 ptr; LEAN_RD16_ZERO(D + pc[0], pc[0], ptr);', 'const u32 sum = ptr + Y;']
        if extra: code.append('if (!lxy && ((sum ^ ptr) & 0x100U)) ++cycle;')
        code.append('const u32 ea = dbr + sum;')
        return Mode(1, 'RAW', code, 3, 'ob', True)
    if name in ('dpindlong', 'dpindlongy'):
        code = ['const u32 zp = (D + pc[0]) & 0xffffU;', 'u32 ptr; LEAN_RD16_ZERO(zp, pc[0], ptr);',
                'u32 bank; LEAN_RD8_ZERO(zp + 2U, ob, bank);',
                'const u32 ea = (ptr | (bank << 16))' + (' + Y;' if name.endswith('y') else ';')]
        return Mode(1, 'RAW', code, 4, 'ob')
    if name == 'srindy':
        return Mode(1, 'RAW', ['u32 ptr; LEAN_RD16_ZERO((S + pc[0]) & 0xffffU, pc[0], ptr);', 'const u32 ea = dbr + ptr + Y;'], 3, 'ob')
    raise KeyError(name)

out = []
labels = {}   # opcode -> ('a'|'x'|None)
def emit(opcode, width_kind, bodies):
    """bodies: {None: lines} or {8: lines, 16: lines}"""
    labels[opcode] = width_kind
    for w, lines in bodies.items():
        suffix = '' if w is None else ('_%s%d' % (width_kind, 0 if w == 8 else 1))
        out.append(f'L_{opcode:02x}{suffix}: {{')
        out.extend('    ' + l for l in lines)
        out.append('}')

# Shared steps. A label is emitted once; what it holds must not depend on the
# opcode that names it.
shared = {}        # label -> lines
order = []
two_step = {}      # opcode -> {8: (first, second), 16: (first, second)}
def step(label, lines):
    if label in shared:
        assert shared[label] == lines, label
    else:
        shared[label] = lines
        order.append(label)
    return label

def ea_code(m):
    """The mode's statements with `ea` as the shared variable. Bit 31 of `ea`
    says its second byte wraps inside bank zero."""
    code = [l.replace('const u32 ea = ', 'ea = ') for l in m.code]
    if m.fam == 'ZERO':
        code.append('ea = (ea & 0xffffU) | 0x80000000U;')
    return code

def read_step(md, extra, w, bump=0, tag=''):
    """Effective address and operand of a read: leaves `ea` and `m`."""
    if md == 'imm':
        n = w // 8
        return step(f'M_imm{w}', ['m = pc[0];' if w == 8 else 'm = pc[0] | (u32(pc[1]) << 8);', f'ob = pc[{n - 1}];',
                                  f'pc += {n};', f'cycle += {1 + n}U;', f'LEAN_PROBE_IP(ip0 + {n + 1}U);', 'goto *entry->second;'])
    m = mode(md, extra)
    name = f'M_{md}{"x" if extra else ""}{tag}_r{w}'
    return step(name, ea_code(m) + [f'LEAN_RD{w}(ea, {m.obx}, m);', f'pc += {m.n};',
                                    f'cycle += {1 + m.cyc + w // 8 + bump}U;', f'LEAN_PROBE_IP(ip0 + {m.n + 1}U);', 'goto *entry->second;'])

def address_step(md, extra, cycles):
    """Effective address of a store: leaves `ea`, and in `ob` what is on the bus."""
    m = mode(md, extra)
    lines = ea_code(m)
    if m.obx != 'ob': lines.append(f'ob = {m.obx};')
    return step(f'M_{md}{"x" if extra else ""}_ea{cycles}', lines + [f'pc += {m.n};', f'cycle += {cycles}U;',
                f'LEAN_PROBE_IP(ip0 + {m.n + 1}U);', 'goto *entry->second;'])

def setnz(w, v): return f'zero = {v}; negative = {v} >> {7 if w == 8 else 15};'
def mask(w): return '0xffU' if w == 8 else '0xffffU'
def get(reg, w): return f'({reg} & 0xffU)' if w == 8 else reg
def put(reg, w, v): return f'{reg} = ({reg} & 0xff00U) | ({v});' if w == 8 else f'{reg} = ({v});'
def rd(fam, w, ea, obx, var): return f'LEAN_RD{w}_{fam}({ea}, {obx}, {var});'
def wr(fam, w, ea, v, obx): return f'LEAN_WR{w}_{fam}({ea}, {v}, {obx});'
def nxt(n, cycles): return [f'cycle += {cycles}U;', f'LEAN_PROBE_IP(ip0 + {n + 1}U);', f'LEAN_NEXT({n});']

def alu_core(mn, w):
    hi = '0x80U' if w == 8 else '0x8000U'
    if mn in ('ORA', 'AND', 'EOR'):
        op = {'ORA': '|', 'AND': '&', 'EOR': '^'}[mn]
        return [f'const u32 r = {get("A", w)} {op} m;', put('A', w, 'r'), setnz(w, 'r')]
    if mn == 'ADC':
        return [f'const u32 reg = {get("A", w)};', 'const u32 result = reg + m + ((carry >> 1) & 1U);',
                f'const u32 cn = result >> {w - 1};', f'zero = result & {mask(w)}; carry = cn; negative = cn;',
                f'other = (other & 0xffbfU) | (((reg ^ result) & ~(reg ^ m) & {hi}) ? 0x40U : 0U);', put('A', w, f'result & {mask(w)}')]
    if mn == 'SBC':
        return [f'const u32 reg = {get("A", w)};', 'const u32 result = reg - m - 1U + ((carry >> 1) & 1U);',
                f'const u32 cn = result >> {w - 1};', 'zero = result; carry = ~cn; negative = cn;',
                f'other = (other & 0xffbfU) | (((reg ^ result) & (reg ^ m) & {hi}) ? 0x40U : 0U);', put('A', w, f'result & {mask(w)}')]
    if mn in ('CMP', 'CPX', 'CPY'):
        reg = {'CMP': 'A', 'CPX': 'X', 'CPY': 'Y'}[mn]
        return [f'const u32 reg = {get(reg, w)};', f'const u32 diff = (reg - m) & {mask(w)};', setnz(w, 'diff'), 'carry = reg >= m ? 2U : 0U;']
    if mn == 'BIT':
        top = 'm' if w == 8 else '(m >> 8)'
        return [f'zero = {get("A", w)} & m;', f'negative = {top} >> 7;', f'other = (other & 0xffbfU) | ({top} & 0x40U);']
    if mn == 'BITIMM':
        return [f'zero = {get("A", w)} & m;']
    if mn in ('LDA', 'LDX', 'LDY'):
        reg = mn[2]
        return [put(reg, w, 'm'), setnz(w, 'm')]
    raise KeyError(mn)

def shift_core(mn, w):
    if mn == 'ASL': return [f'const u32 b = (m << 1) & {mask(w)};', setnz(w, 'b'), f'carry = m >> {w - 2};']
    if mn == 'LSR': return ['const u32 b = m >> 1;', 'zero = m >> 1; negative = 0U; carry = (m & 1U) << 1;']
    if mn == 'ROR': return [f'const u32 b = ((m >> 1) | ((carry & 2U) << {w - 2})) & {mask(w)};', setnz(w, 'b'), 'carry = (m & 1U) << 1;']
    if mn == 'ROL': return [f'const u32 b = ((m << 1) | ((carry & 2U) >> 1)) & {mask(w)};', setnz(w, 'b'), f'carry = m >> {w - 2};']
    if mn == 'INC': return [f'const u32 b = (m + 1U) & {mask(w)};', setnz(w, 'b')]
    if mn == 'DEC': return [f'const u32 b = (m - 1U) & {mask(w)};', setnz(w, 'b')]
    raise KeyError(mn)

for opcode in range(256):
    mn, md = T[opcode][0], T[opcode][1]
    extra = T[opcode][2] if len(T[opcode]) > 2 else False
    kind = 'a' if mn in A_OPS else 'x' if mn in XY_OPS else None
    op_const = f'0x{opcode:02x}U'
    # ---- reads through an addressing mode
    if mn in ('ORA', 'AND', 'EOR', 'ADC', 'SBC', 'CMP', 'BIT', 'BITIMM', 'LDA', 'LDX', 'LDY', 'CPX', 'CPY'):
        # ADC and SBC in decimal mode are the library's: the table rows for
        # that mode send them to L_leave before anything is read.
        labels[opcode] = kind
        two_step[opcode] = {}
        for w in (8, 16):
            first = read_step(md, extra, w)
            second = step(f'O_{mn}{w}', alu_core(mn, w) + ['LEAN_DISPATCH;'])
            two_step[opcode][w] = (first, second)
    elif mn in ('STA', 'STX', 'STY', 'STZ'):
        labels[opcode] = kind
        two_step[opcode] = {}
        for w in (8, 16):
            m = mode(md, extra)
            value = '0U' if mn == 'STZ' else get(mn[2], w)
            first = address_step(md, extra, 1 + m.cyc + (1 if (m.maybe and mn != 'STZ') else 0))
            second = step(f'O_{mn}{w}', [f'LEAN_WR{w}(ea, {value}, ob);', f'cycle += {w // 8}U;', 'LEAN_AFTER_WRITE;'])
            two_step[opcode][w] = (first, second)
    elif mn in ('TSB', 'TRB', 'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC') and md != 'acc':
        labels[opcode] = kind
        two_step[opcode] = {}
        for w in (8, 16):
            m = mode(md, extra)
            # An indexed absolute modify always spends the cycle a read spends
            # only when the index carries.
            first = read_step(md, extra, w, 1, 'rmw') if m.maybe else read_step(md, extra, w)
            if mn in ('TSB', 'TRB'):
                assert not m.maybe
                lines = [f'const u32 a = {get("A", w)}; zero = a & m;',
                         f'const u32 b = (m | a) & {mask(w)};' if mn == 'TSB' else f'const u32 b = m & ~a & {mask(w)};']
                internal = 0
            else:
                lines = shift_core(mn, w)
                internal = 1
            second = step(f'O_{mn}{w}', lines + [f'LEAN_WR{w}(ea, b, ob);', f'cycle += {w // 8 + internal}U;', 'LEAN_AFTER_WRITE;'])
            two_step[opcode][w] = (first, second)
    elif mn in ('ASL', 'LSR', 'ROL', 'ROR') and md == 'acc':
        bodies = {}
        for w in (8, 16):
            lines = [f'const u32 m = {get("A", w)};'] + shift_core(mn, w) + [put('A', w, 'b'), f'ob = {op_const};'] + nxt(0, 2)
            bodies[w] = lines
        emit(opcode, 'a', bodies)
    elif mn in ('INA', 'DEA', 'INX', 'INY', 'DEX', 'DEY'):
        reg = {'INA': 'A', 'DEA': 'A', 'INX': 'X', 'DEX': 'X', 'INY': 'Y', 'DEY': 'Y'}[mn]
        delta = '+ 1U' if mn[0] == 'I' else '- 1U'
        bodies = {}
        for w in (8, 16):
            bodies[w] = [f'const u32 b = ({get(reg, w)} {delta}) & {mask(w)};', put(reg, w, 'b'), setnz(w, 'b'), f'ob = {op_const};'] + nxt(0, 2)
        emit(opcode, 'a' if reg == 'A' else 'x', bodies)
    elif mn in ('TAX', 'TAY', 'TSX', 'TXA', 'TXY', 'TYA', 'TYX'):
        src = {'TAX': 'A', 'TAY': 'A', 'TSX': 'S', 'TXA': 'X', 'TXY': 'X', 'TYA': 'Y', 'TYX': 'Y'}[mn]
        dst = mn[2]
        bodies = {}
        for w in (8, 16):
            bodies[w] = [f'const u32 v = {get(src, w)};', put(dst, w, 'v'), setnz(w, 'v'), f'ob = {op_const};'] + nxt(0, 2)
        emit(opcode, 'a' if dst == 'A' else 'x', bodies)
    elif mn in ('TCD', 'TDC', 'TSC'):
        src, dst = {'TCD': ('A', 'D'), 'TDC': ('D', 'A'), 'TSC': ('S', 'A')}[mn]
        emit(opcode, None, {None: [f'{dst} = {src};', setnz(16, dst), f'ob = {op_const};'] + nxt(0, 2)})
    elif mn in ('TXS', 'TCS'):
        emit(opcode, None, {None: [f'S = {"X" if mn == "TXS" else "A"};', f'ob = {op_const};'] + nxt(0, 2)})
    elif mn in ('CLC', 'SEC', 'CLI', 'SEI', 'CLD', 'SED', 'CLV'):
        stmt = {'CLC': 'carry = 0U;', 'SEC': 'carry = 2U;', 'CLI': 'interrupts = 1U;', 'SEI': 'interrupts = 0U;',
                'CLD': 'other &= 0xfff7U; LEAN_UPDATE_MODE;', 'SED': 'other |= 8U; LEAN_UPDATE_MODE;', 'CLV': 'other &= 0xffbfU;'}[mn]
        emit(opcode, None, {None: [stmt, f'ob = {op_const};'] + nxt(0, 2)})
    elif mn == 'NOP':
        emit(opcode, None, {None: [f'ob = {op_const};'] + nxt(0, 2)})
    elif mn == 'WDM':
        emit(opcode, None, {None: [f'ob = {op_const};'] + nxt(1, 1)})
    elif mn == 'XBA':
        emit(opcode, None, {None: ['A = ((A >> 8) | (A << 8)) & 0xffffU;', 'zero = A & 0xffU; negative = (A & 0xffU) >> 7;', f'ob = {op_const};'] + nxt(0, 1)})
    elif mn in ('REP', 'SEP'):
        # Nearly every REP and SEP in the cartridge only changes register widths.
        if mn == 'REP':
            lines = ['const u32 flags = pc[0];', 'other &= ~flags;',
                     'if (LEAN_UNLIKELY(flags & 0xcfU)) {',
                     '    zero |= flags & 2U; if (flags & 1U) carry = 0U;',
                     '    if (flags & 4U) interrupts = 1U; if (flags & 0x80U) negative = 0U;', '}',
                     'if (flags & 0x10U) lxy = 1U;', 'if (flags & 0x20U) la = 1U;',
                     'if (flags & 0x38U) table = LEAN_TABLE();']
        else:
            lines = ['const u32 flags = pc[0];', 'other |= flags;',
                     'if (LEAN_UNLIKELY(flags & 0xcfU)) {',
                     '    if (flags & 1U) carry = 2U; if (flags & 2U) zero = 0U;',
                     '    if (flags & 4U) interrupts = 0U; if (flags & 0x80U) negative = 1U;', '}',
                     'if (flags & 0x10U) { lxy = 0U; X &= 0xffU; Y &= 0xffU; }', 'if (flags & 0x20U) la = 0U;',
                     'if (flags & 0x38U) table = LEAN_TABLE();']
        emit(opcode, None, {None: lines + ['ob = flags;'] + nxt(1, 2)})
    elif mn in ('PHA', 'PHX', 'PHY'):
        reg = mn[2]
        bodies = {}
        for w in (8, 16):
            bodies[w] = [f'LEAN_PUSH{w}({get(reg, w)}, {op_const});', f'cycle += {2 + w // 8}U;', 'LEAN_PROBE_IP(ip0 + 1U);', 'LEAN_AFTER_WRITE;']
        emit(opcode, 'a' if reg == 'A' else 'x', bodies)
    elif mn == 'PHD':
        emit(opcode, None, {None: [f'LEAN_PUSH16(D, {op_const});', 'cycle += 4U;', 'LEAN_PROBE_IP(ip0 + 1U);', 'LEAN_AFTER_WRITE;']})
    elif mn in ('PHK', 'PHB'):
        value = '(pbr >> 16)' if mn == 'PHK' else '(dbr >> 16)'
        emit(opcode, None, {None: [f'LEAN_PUSH8({value}, {op_const});', f'cycle += {3 if mn == "PHK" else 2}U;', 'LEAN_PROBE_IP(ip0 + 1U);', 'LEAN_AFTER_WRITE;']})
    elif mn == 'PHP':
        emit(opcode, None, {None: [f'LEAN_PUSH8(LEAN_STATUS(), {op_const});', 'cycle += 3U;', 'LEAN_PROBE_IP(ip0 + 1U);', 'LEAN_AFTER_WRITE;']})
    elif mn in ('PLA', 'PLX', 'PLY'):
        reg = mn[2]
        bodies = {}
        for w in (8, 16):
            bodies[w] = [f'u32 v; LEAN_POP{w}(v, {op_const});', put(reg, w, 'v'), setnz(w, 'v')] + nxt(0, 3 + w // 8)
        emit(opcode, 'a' if reg == 'A' else 'x', bodies)
    elif mn == 'PLD':
        emit(opcode, None, {None: [f'u32 v; LEAN_POP16(v, {op_const});', 'D = v;', setnz(16, 'v')] + nxt(0, 5)})
    elif mn == 'PLB':
        emit(opcode, None, {None: [f'u32 v; LEAN_POP8(v, {op_const});', 'dbr = v << 16;', setnz(8, 'v')] + nxt(0, 4)})
    elif mn == 'PLP':
        emit(opcode, None, {None: [f'u32 v; LEAN_POP8(v, {op_const});', 'LEAN_SET_STATUS(v);'] + nxt(0, 4)})
    elif mn == 'PEA':
        emit(opcode, None, {None: ['LEAN_PUSH16(pc[0] | (u32(pc[1]) << 8), pc[1]);', 'cycle += 5U;', 'LEAN_PROBE_IP(ip0 + 3U);', 'pc += 2;', 'LEAN_AFTER_WRITE;']})
    elif mn == 'PEI':
        emit(opcode, None, {None: ['if (D & 0xffU) ++cycle;', 'u32 v; LEAN_RD16_ZERO(D + pc[0], pc[0], v);', 'LEAN_PUSH16(v, ob);',
                                   'cycle += 6U;', 'LEAN_PROBE_IP(ip0 + 2U);', 'pc += 1;', 'LEAN_AFTER_WRITE;']})
    elif mn == 'PER':
        emit(opcode, None, {None: ['const u32 v = ((pc[0] | (u32(pc[1]) << 8)) + LEAN_IP() + 3U) & 0xffffU;', 'LEAN_PUSH16(v, pc[1]);',
                                   'cycle += 5U;', 'LEAN_PROBE_IP(ip0 + 3U);', 'pc += 2;', 'LEAN_AFTER_WRITE;']})
    elif mn in ('BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ', 'BRA'):
        cond = {'BPL': '!(negative & 1U)', 'BMI': '(negative & 1U)', 'BVC': '!(other & 0x40U)', 'BVS': '(other & 0x40U)',
                'BCC': '!(carry & 2U)', 'BCS': '(carry & 2U)', 'BNE': 'zero', 'BEQ': '!zero', 'BRA': '1'}[mn]
        emit(opcode, None, {None: ['ob = pc[0];', f'if ({cond}) {{', '    cycle += 3U;',
                                   '    LEAN_PROBE_IP(ip0 + (pc[0] & 0x80U ? (pc[0] | 0xff00U) : pc[0]) + 2U);',
                                   '    LEAN_BRANCH(static_cast<std::int8_t>(pc[0]) + 1);', '}'] + nxt(1, 2)})
    elif mn == 'BRL':
        emit(opcode, None, {None: ['ob = pc[1];', 'cycle += 3U;', 'const u32 offset = pc[0] | (u32(pc[1]) << 8);',
                                   'LEAN_PROBE_IP(ip0 + offset + 3U);', 'LEAN_JUMP_NEAR((LEAN_IP() + offset + 3U) & 0xffffU);']})
    elif mn == 'JMP':
        emit(opcode, None, {None: ['ob = pc[1];', 'cycle += 3U;', 'const u32 target = pc[0] | (u32(pc[1]) << 8);',
                                   'LEAN_PROBE_IP(target + dbr);', 'LEAN_JUMP_NEAR(target);']})
    elif mn == 'JML':
        emit(opcode, None, {None: ['ob = pc[2];', 'cycle += 4U;', 'const u32 target = pc[0] | (u32(pc[1]) << 8) | (u32(pc[2]) << 16);',
                                   'LEAN_PROBE_IP(target);', 'pbr = target & 0xff0000U;', 'LEAN_JUMP_FAR(target & 0xffffU);']})
    elif mn == 'JMPIND':
        emit(opcode, None, {None: ['u32 target; LEAN_RD16_ZERO(pc[0] | (u32(pc[1]) << 8), pc[1], target);', 'cycle += 5U;',
                                   'LEAN_PROBE_IP(target);', 'LEAN_JUMP_NEAR(target);']})
    elif mn == 'JMLIND':
        emit(opcode, None, {None: ['const u32 at = pc[0] | (u32(pc[1]) << 8);', 'u32 target; LEAN_RD16_ZERO(at, pc[1], target);',
                                   'u32 bank; LEAN_RD8_ZERO(at + 2U, ob, bank);', 'cycle += 6U;', 'LEAN_PROBE_IP(target + (bank << 16));',
                                   'pbr = bank << 16;', 'LEAN_JUMP_FAR(target);']})
    elif mn in ('JMPINDX', 'JSRINDX'):
        lines = ['const u32 at = pbr | (((pc[0] | (u32(pc[1]) << 8)) + X) & 0xffffU);', 'u32 target; LEAN_RD16_RAW(at, pc[1], target);']
        if mn == 'JSRINDX':
            lines += ['LEAN_PUSH16((LEAN_IP() + 2U) & 0xffffU, ob);', 'cycle += 8U;']
        else:
            lines += ['cycle += 5U;']
        lines += ['LEAN_PROBE_IP(target);', 'LEAN_JUMP_NEAR(target);']
        emit(opcode, None, {None: lines})
    elif mn == 'JSR':
        emit(opcode, None, {None: ['const u32 target = pc[0] | (u32(pc[1]) << 8);', 'LEAN_PUSH16((LEAN_IP() + 2U) & 0xffffU, pc[1]);',
                                   'cycle += 6U;', 'LEAN_PROBE_IP(target + dbr);', 'LEAN_JUMP_NEAR(target);']})
    elif mn == 'JSL':
        emit(opcode, None, {None: ['const u32 target = pc[0] | (u32(pc[1]) << 8) | (u32(pc[2]) << 16);', 'LEAN_PUSH8(pbr >> 16, pc[2]);',
                                   'LEAN_PUSH16((LEAN_IP() + 3U) & 0xffffU, ob);', 'cycle += 8U;', 'LEAN_PROBE_IP(target);',
                                   'pbr = target & 0xff0000U;', 'LEAN_JUMP_FAR(target & 0xffffU);']})
    elif mn == 'RTS':
        emit(opcode, None, {None: [f'u32 v; LEAN_POP16(v, {op_const});', 'cycle += 6U;', 'LEAN_PROBE_IP(v + 1U);', 'LEAN_JUMP_NEAR((v + 1U) & 0xffffU);']})
    elif mn == 'RTL':
        emit(opcode, None, {None: [f'u32 v; LEAN_POP16(v, {op_const});', 'u32 bank; LEAN_POP8(bank, ob);', 'cycle += 6U;',
                                   'LEAN_PROBE_IP(v + 1U);', 'pbr = bank << 16;', 'LEAN_JUMP_FAR((v + 1U) & 0xffffU);']})
    elif mn == 'RTI':
        emit(opcode, None, {None: [f'u32 flags; LEAN_POP8(flags, {op_const});', 'LEAN_SET_STATUS(flags);', 'u32 v; LEAN_POP16(v, ob);',
                                   'u32 bank; LEAN_POP8(bank, ob);', 'cycle += 7U;', 'LEAN_PROBE_IP(v);', 'pbr = bank << 16;', 'LEAN_JUMP_FAR(v);']})
    elif mn in ('MVN', 'MVP'):
        inc = '1U' if mn == 'MVN' else '0xffffU'
        emit(opcode, None, {None: ['if (A == 0xffffU) {', '    ob = pc[1]; cycle += 3U; LEAN_PROBE_IP(ip0 + 3U); LEAN_NEXT(2);', '}',
                                   'const u32 source = (u32(pc[1]) << 16) | X; const u32 destination = (u32(pc[0]) << 16) | Y;',
                                   f'X = (X + {inc}) & 0xffffU; Y = (Y + {inc}) & 0xffffU; A = (A - 1U) & 0xffffU;',
                                   'u32 byte; LEAN_RD8_RAW(source, pc[1], byte);', 'LEAN_WR8_RAW(destination, byte, ob);',
                                   'dbr = u32(pc[0]) << 16;', 'cycle += 5U;', 'LEAN_PROBE_IP(ip0);', 'pc -= 1;', 'LEAN_AFTER_WRITE_AGAIN;']})
    elif mn in ('BRK', 'COP', 'WAI', 'STP', 'XCE'):
        emit(opcode, None, {None: ['LEAN_LEAVE;']})
    else:
        raise SystemExit(f'unhandled {opcode:02x} {mn} {md}')

out.append('L_leave: {')
out.append('    LEAN_LEAVE;')
out.append('}')
steps = []
for label in order:
    steps.append(f'{label}: {{')
    steps.extend('    ' + l for l in shared[label])
    steps.append('}')
# Rows: 16-bit accumulator, plus two for 16-bit index registers, plus four for
# decimal mode.
table = ['static const LeanEntry lean_table[8][256] = {']
for m in range(8):
    row_ = []
    for opcode in range(256):
        kind = labels[opcode]
        w = None if kind is None else (16 if ((m & 1) if kind == 'a' else ((m >> 1) & 1)) else 8)
        if (m & 4) and T[opcode][0] in ('ADC', 'SBC'): row_.append('{&&L_leave, nullptr}')
        elif opcode in two_step: row_.append('{&&%s, &&%s}' % two_step[opcode][w])
        elif kind is None: row_.append(f'{{&&L_{opcode:02x}, nullptr}}')
        elif kind == 'a': row_.append(f'{{&&L_{opcode:02x}_a{m & 1}, nullptr}}')
        else: row_.append(f'{{&&L_{opcode:02x}_x{(m >> 1) & 1}, nullptr}}')
    table.append('    {' + ', '.join(row_) + '},')
table.append('};')
length = ['// Instruction lengths per mode (opcode plus operand bytes).', 'static const std::uint8_t lean_length[4][256] = {']
for m in range(4):
    row_ = []
    for opcode in range(256):
        mn, md = T[opcode][0], T[opcode][1]
        if md == 'imm': n = (2 if (m & 1) else 1) if mn in A_OPS else (2 if (m >> 1) else 1)
        elif md in ('abs', 'absx', 'absy'): n = 2
        elif md in ('long', 'longx'): n = 3
        elif md is not None and md != 'acc': n = 1
        else:
            n = {'JSR': 2, 'JMP': 2, 'JMPIND': 2, 'JMPINDX': 2, 'JSRINDX': 2, 'JMLIND': 2, 'JSL': 3, 'JML': 3, 'BRL': 2, 'PER': 2, 'PEA': 2,
                 'MVN': 2, 'MVP': 2, 'REP': 1, 'SEP': 1, 'PEI': 1, 'WDM': 1, 'BRK': 1, 'COP': 1}.get(mn, 1 if mn in ('BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ', 'BRA') else 0)
        row_.append(str(n + 1))
    length.append('    {' + ','.join(row_) + '},')
length.append('};')
with open(sys.argv[1], 'w') as f:
    f.write('// Generated by tools/generate-lean-65816.py. Do not edit.\n')
    f.write('#if defined(LEAN_LENGTHS)\n' + '\n'.join(length) + '\n#elif defined(LEAN_TABLES)\n' + '\n'.join(table)
            + '\n#else\n' + '\n'.join(steps + out) + '\n#endif\n')
print(f'{len(steps) + len(out)} lines, {len(order)} shared steps, {sum(1 for l in out if l.startswith("L_"))} whole handlers')
