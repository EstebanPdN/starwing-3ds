#!/usr/bin/env python3
"""Check the exact tracked publication selection without printing sensitive data."""
from __future__ import annotations

import fnmatch
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = (
    '*.sfc', '*.smc', '*.rom', '*.bin', '*.bps', '*.pak', '*.cia', '*.3dsx',
    '*.elf', '*.o', '*.a', '*.srm', '*.sav', '*.state', '*.dmp', '*.log',
    '*.partial', '*.pem', '*.key', '*.p12', '*.pfx', '*.pyc',
    '*title_logos.hpp', '*dialogue_catalog.hpp', '*_art_data.hpp',
    '*dialogue.json', '.env', '.env.*', 'AGENTS.md', '.DS_Store',
)
PRIVATE_COMPONENTS = {'private', 'dumps', 'roms', 'dist', '__pycache__', 'sdmc:'}
SECRETS = re.compile(
    rb'gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,}'
    rb'|sk-(?:proj-)?[A-Za-z0-9_-]{32,}'
    rb'|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'
)


def main() -> int:
    names = subprocess.check_output(
        ['git', '-C', str(ROOT), 'ls-files', '-z']).decode().split('\0')
    names = [name for name in names if name]
    stages = subprocess.check_output(
        ['git', '-C', str(ROOT), 'ls-files', '--stage', '-z']).decode()
    gitlinks = {row.split('\t', 1)[1]: row.split()[1]
                for row in stages.split('\0') if row.startswith('160000 ')}
    if not names:
        raise SystemExit('No tracked publication selection; stage the reviewed files first.')
    problems = []
    for name in names:
        path = Path(name)
        lowered = path.name.lower()
        if any(fnmatch.fnmatch(lowered, p.lower()) for p in FORBIDDEN):
            problems.append(f'Excluded file is tracked: {name}')
        if PRIVATE_COMPONENTS.intersection(path.parts):
            problems.append(f'Private directory is tracked: {name}')
        target = ROOT / path
        if name in gitlinks:
            import json
            metadata = json.loads((ROOT / 'config/upstream-3ds.json').read_text())
            if name != 'upstream-ultrastarfox' or gitlinks[name] != metadata['ultrastarfox_commit']:
                problems.append(f'Unreviewed submodule reference: {name}')
            continue
        if target.is_symlink():
            problems.append(f'Symlink needs a separate review: {name}')
            continue
        data = target.read_bytes()
        if SECRETS.search(data):
            problems.append(f'Possible secret; inspect locally: {name}')
        if b'/Users/' in data or b'/home/esteban/' in data:
            # The checker stores its own detection rules, not a private path.
            if name != 'tools/check-publication.py':
                problems.append(f'Personal absolute path: {name}')
        if len(data) > 5 * 1024 * 1024:
            problems.append(f'Unexpected large source file: {name}')
        if b'\0' in data and name != 'showcase.png':
            problems.append(f'Unexpected binary payload: {name}')
    version = (ROOT / 'platform/3ds/version.txt').read_text().strip()
    cia = (ROOT / 'platform/3ds/cia-version.txt').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+(?:\.\d+)*', version):
        problems.append('Invalid source version.')
    if not cia.isdigit() or not 0 <= int(cia) <= 65535:
        problems.append('Invalid CIA title version.')
    script = (ROOT / 'platform/3ds/build-game.sh').read_text()
    cmake = (ROOT / 'platform/3ds/CMakeLists.txt').read_text()
    source = (ROOT / 'platform/3ds/source/game_3ds.cpp').read_text()
    if 'platform/3ds/version.txt' not in script or 'version.txt' not in cmake:
        problems.append('Build metadata is not using the shared version file.')
    if 'build_version = STARWING_BUILD_VERSION' not in source:
        problems.append('Runtime version is not using the CMake build definition.')
    status = (ROOT / 'docs/STATUS.md').read_text()
    if f'Current development version: **{version}**' not in status:
        problems.append('Documented development version differs from source metadata.')
    for name in names:
        if not name.endswith('.md'):
            continue
        body = (ROOT / name).read_text()
        for link in re.findall(r'\]\(([^\s)]+)\)', body):
            if '://' in link or link.startswith('#'):
                continue
            local = link.split('#', 1)[0]
            if local and not (ROOT / name).parent.joinpath(local).exists():
                problems.append(f'Broken local link in {name}: {local}')
    if problems:
        raise SystemExit('\n'.join(problems))
    print(f'PASS: {len(names)} tracked files; private-data and secret checks; '
          f'version {version}; CIA title version {cia}; local Markdown links.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
