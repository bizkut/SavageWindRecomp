#!/usr/bin/env python3
# Generate the PS5 runtime export table from the game archive and generator sources.
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def symbols(nm, paths):
    defined = {}
    undefined = set()
    for path in paths:
        proc = subprocess.run([nm, '-g', '-P', str(path)], check=True, stdout=subprocess.PIPE, text=True)
        for line in proc.stdout.splitlines():
            fields = line.split()
            if len(fields) < 2 or len(fields[1]) != 1:
                continue
            name, kind = fields[:2]
            if kind in ('U', 'w', 'v'):
                undefined.add(name)
            elif kind.isupper() and kind not in ('N',):
                defined[name] = kind
    return defined, undefined


def emitted_names():
    files = list((ROOT / 'tools/gen_cpp').glob('lift*.h'))
    files += list((ROOT / 'tools/gen_cpp').glob('*_local.h'))
    files += [ROOT / 'tools/gen_cpp/emit.cpp', ROOT / 'tools/gen_cpp/patches_generated.h']
    names = set()
    for path in files:
        text = path.read_text()
        # The generated C++ splits adjacent string literals at arbitrary columns, including inside identifiers.
        text = re.sub(r'"\s*"', '', text)
        names.update(re.findall(r'\brt_[A-Za-z0-9_]+', text))
    # These helpers are static inline in the game code headers, never external relocations.
    names.difference_update(('rt_fpcr_write', 'rt_fsub_s'))
    for line in (ROOT / 'runtime/hle/hle_symbols.txt').read_text().splitlines():
        name = line.strip()
        if name and not name.startswith('#'):
            names.add('hle_' + re.sub(r'[^A-Za-z0-9_]', '_', name))
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--nm', required=True)
    parser.add_argument('--archive', action='append', default=[])
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--object', action='append', default=[])
    parser.add_argument('--library', action='append', default=[])
    parser.add_argument('--output', required=True)
    parser.add_argument('--abi', required=True)
    args = parser.parse_args()
    if not args.archive and not args.manifest:
        parser.error('at least one --archive or --manifest is required')
    game_defs, game_undef = symbols(args.nm, args.archive)
    required = (game_undef - game_defs.keys()) | emitted_names()
    if args.manifest:
        required.update(line.strip() for line in args.manifest.read_text().splitlines()
                        if line.strip() and not line.lstrip().startswith('#'))
    runtime_defs, _ = symbols(args.nm, args.object + args.library)
    missing = sorted(required - runtime_defs.keys())
    if missing:
        print('gamecode exports missing runtime definitions: ' + ', '.join(missing), file=sys.stderr)
        return 1
    names = sorted(required)
    if any(not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name) for name in names):
        raise ValueError('unsupported export symbol spelling')
    h = hashlib.sha256()
    h.update(b'gamecode-exports-v1\0')
    h.update(args.abi.encode() + b'\0')
    for name in names:
        h.update(name.encode() + b'\0' + runtime_defs[name].encode() + b'\0')
    # Object contents capture runtime ABI/implementation changes; paths are not part of the identity.
    for path in args.object + args.library:
        h.update(hashlib.sha256(Path(path).read_bytes()).digest())
    digest = h.hexdigest()
    lines = ['// Generated; do not edit.', '#include "gamecode_exports.h"', '']
    for index, name in enumerate(names):
        lines.append(f'extern char export_{index}[] __asm__("{name}");')
    lines += ['', 'const GameExport game_exports[] = {']
    lines += [f'    {{"{name}", (void *)export_{index}}},' for index, name in enumerate(names)]
    lines += ['};', f'const size_t game_export_count = {len(names)};',
              f'const char game_export_abi_digest[] = "{digest}";', '']
    Path(args.output).write_text('\n'.join(lines))
    print(f'gamecode exports: {len(names)} sha256:{digest}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
