#!/usr/bin/env python3
"""Validate serialized CGFX Patricia dictionaries, not physical compatibility."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def validate(data: bytes) -> dict:
    report = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
              'dictionaries': 0, 'entries': 0, 'failures': []}
    errors = report['failures']
    if len(data) < 20 or data[:4] != b'CGFX':
        errors.append('Missing CGFX header')
        return report
    if struct.unpack_from('<H', data, 4)[0] != 0xFEFF:
        errors.append('Unsupported CGFX byte order')
        return report
    if struct.unpack_from('<I', data, 12)[0] != len(data):
        errors.append('CGFX declared size differs from file length')
    # Scan structurally identifiable aligned DICT tables. This is deliberately
    # not advertised as validation of every possible CGFX pointer or GPU command.
    for off in range(0, len(data) - 11, 4):
        if data[off:off + 4] != b'DICT':
            continue
        size, count = struct.unpack_from('<II', data, off + 4)
        if size != 12 + 16 * (count + 1):
            continue
        label = f'DICT@0x{off:x}'
        report['dictionaries'] += 1
        report['entries'] += count
        if off + size > len(data):
            errors.append(f'{label}: node table extends beyond EOF')
            continue
        rows = []
        try:
            for i in range(count + 1):
                pos = off + 12 + 16 * i
                bit, left, right, namep, objp = struct.unpack_from('<IHHii', data, pos)
                if left > count or right > count:
                    raise ValueError(f'node {i}: child index out of range')
                namepos = pos + 8 + namep
                if namep:
                    if not 0 <= namepos < len(data):
                        raise ValueError(f'node {i}: name pointer out of range')
                    end = data.find(b'\0', namepos)
                    if end < 0:
                        raise ValueError(f'node {i}: unterminated name')
                    name = data[namepos:end]
                else:
                    name = b''
                if i and not name:
                    raise ValueError(f'node {i}: missing name')
                if objp and not 0 <= pos + 12 + objp < len(data):
                    raise ValueError(f'node {i}: object pointer out of range')
                rows.append((bit, left, right, name))
            if rows[0][0] != 0xFFFFFFFF:
                raise ValueError('root reference bit is not 0xFFFFFFFF')
            if len({r[3] for r in rows[1:]}) != count:
                raise ValueError('duplicate dictionary names')
            for expected in range(1, count + 1):
                key = rows[expected][3]
                parent, current = 0, rows[0][1]
                for _ in range(count + 2):
                    if rows[parent][0] <= rows[current][0]:
                        break
                    parent = current
                    bit = rows[current][0]
                    byte = key[bit // 8] if bit // 8 < len(key) else 0
                    current = rows[current][2 if ((byte >> (bit % 8)) & 1) else 1]
                else:
                    raise ValueError(f'node {expected}: nonterminating lookup')
                if current != expected:
                    errors.append(f'{label}: lookup {key!r} resolves node {current} {rows[current][3]!r}, expected {expected}')
        except (ValueError, struct.error) as exc:
            errors.append(f'{label}: {exc}')
    if not report['dictionaries']:
        errors.append('No structurally identifiable DICT found')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    failed = False
    for path in args.paths:
        try:
            report = validate(path.read_bytes())
        except OSError as exc:
            report = {'failures': [str(exc)]}
        report['path'] = str(path)
        print(json.dumps(report, indent=2))
        failed |= bool(report['failures'])
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
