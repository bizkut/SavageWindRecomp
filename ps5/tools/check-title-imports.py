#!/usr/bin/env python3
# Verify native title imports against their actual module providers.
# SPDX-License-Identifier: GPL-3.0-or-later
import base64
import hashlib
import re
import subprocess
import sys
from pathlib import Path

SUFFIX = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.PIPE)


def nid(name):
    digest = hashlib.sha1(name.encode() + SUFFIX).digest()[:8][::-1]
    return base64.b64encode(digest).decode()[:11].replace('/', '-')


def main():
    if len(sys.argv) < 4:
        raise SystemExit('usage: check-title-imports.py <pie.elf> <api-surface.txt> <stub.so>...')
    image, manifest = sys.argv[1:3]
    modules = re.findall(r'Shared library: \[(.*?)\]', command('readelf', '-dW', image))
    stubs = {}
    for path in sys.argv[3:]:
        text = command('readelf', '-dW', path)
        match = re.search(r'\(SONAME\).*\[(.*?)\]', text)
        if match and match.group(1) in modules:
            if match.group(1) in stubs:
                raise SystemExit('duplicate stub provider: ' + match.group(1))
            stubs[match.group(1)] = path
    absent = [name for name in modules if name not in stubs]
    if absent:
        raise SystemExit('missing loaded module stubs: ' + ', '.join(absent))
    exports = {}
    for name in modules:
        lines = command('nm', '-D', '--defined-only', stubs[name]).splitlines()
        exports[name] = {line.split()[-1] for line in lines if line.split()}
    surface = {line.split('|', 1)[0] for line in Path(manifest).read_text().splitlines()
               if line and not line.startswith('#')}
    missing = []
    for line in command('readelf', '--dyn-syms', '-W', image).splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[6] != 'UND':
            continue
        name = fields[7]
        provider = next((module for module in modules if name in exports[module]), None)
        if provider is None or (provider == 'libc.prx' and nid(name) not in surface):
            missing.append((name, provider or 'no provider', nid(name)))
    if missing:
        for name, provider, symbol_nid in missing:
            print(f'missing title import: {name} ({provider}, NID {symbol_nid})', file=sys.stderr)
        return 1
    print(f'title imports checked against SDK module stubs and libc.prx surface: {len(modules)} needed modules')
    return 0


if __name__ == '__main__':
    sys.exit(main())
