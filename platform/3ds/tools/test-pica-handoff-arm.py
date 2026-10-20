#!/usr/bin/env python3
"""Replay the compiled PICA-to-Citro2D handoff with page zero unmapped.

Requires pyelftools and unicorn. Pass a known failing ELF and the candidate ELF.
This exercises ARM CPU code and the linked Citro3D TexBind implementation; it
is not a GPU or physical-console test. No ROM or crash dump is needed.
"""
import argparse
import json
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_R4


def replay(path):
    with open(path, 'rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()}
        draw = next(s for n, s in symbols.items() if '4Pica4drawE' in n)
        cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        mapped = set()
        for segment in elf.iter_segments():
            if segment['p_type'] != 'PT_LOAD':
                continue
            start, size = segment['p_vaddr'], segment['p_memsz']
            for page in range(start & ~4095, (start + size + 4095) & ~4095, 4096):
                if page not in mapped:
                    cpu.mem_map(page, 4096)
                    mapped.add(page)
            cpu.mem_write(start, segment.data())
        start, size = draw['st_value'], draw['st_size']
        blend = symbols['C3D_AlphaBlend']['st_value']
        prepare = symbols['C2D_Prepare']['st_value']
        context = symbols['__C3D_Context']['st_value']
        # Locate the final AlphaBlend call: the following instructions are
        # the exact handoff that faulted, independent of release addresses.
        calls = []
        for offset in range(0, size, 4):
            word = struct.unpack('<I', cpu.mem_read(start + offset, 4))[0]
            if word >> 24 != 0xeb:
                continue
            displacement = word & 0xffffff
            if displacement & 0x800000:
                displacement -= 0x1000000
            if start + offset + 8 + displacement * 4 == blend:
                calls.append(start + offset)
        assert len(calls) == 2, 'Unexpected handoff layout; inspect the generated code'
        cpu.mem_write(context + 32, struct.pack('<I', 1))  # C3DiF_Active
        cpu.mem_map(0x08000000, 0x10000)
        cpu.reg_write(UC_ARM_REG_SP, 0x08008000)
        cpu.reg_write(UC_ARM_REG_R4, 1)
        result = {'reached_citro2d': False, 'fault_address': None, 'fault_pc': None}

        def code(uc, address, size, data):
            if address == prepare:
                result['reached_citro2d'] = True
                uc.emu_stop()

        def fault(uc, access, address, size, value, data):
            result['fault_address'] = address
            result['fault_pc'] = uc.reg_read(UC_ARM_REG_PC)
            return False

        cpu.hook_add(UC_HOOK_CODE, code)
        cpu.hook_add(UC_HOOK_MEM_INVALID, fault)
        try:
            cpu.emu_start(calls[-1] + 4, 0, count=1000)
        except UcError:
            if result['fault_address'] is None:
                raise
        return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline_elf')
    parser.add_argument('candidate_elf')
    args = parser.parse_args()
    before, after = replay(args.baseline_elf), replay(args.candidate_elf)
    assert before['fault_address'] == 12 and not before['reached_citro2d'], before
    assert after['fault_address'] is None and after['reached_citro2d'], after
    print(json.dumps({'baseline': before, 'candidate': after, 'passed': True}, indent=2))
