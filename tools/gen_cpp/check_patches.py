#!/usr/bin/env python3
"""Compare every main patch and instruction-bit mutation with the Python implementation."""
import pathlib
import struct
import subprocess
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import game_sites
import nso
import patches

module = nso.Nso(sys.argv[1])
version, sites = game_sites.for_module(module, game_sites.MAIN_BASE)
if version != '1.9.0':
    raise SystemExit(f'expected 1.9.0, got {version}')
image = bytearray(module.image)
image_path = pathlib.Path('build/s9-cpp/patches-image.bin')
image_path.write_bytes(image)
gen = SimpleNamespace(nso=SimpleNamespace(image=image), base=game_sites.MAIN_BASE, sites=sites)
commands = [str(len(sites))] + [f'{name} {value:x}' for name, value in sites.items()]
expected = []
for name, patch in patches.PATCHES.items():
    if name not in sites:
        raise SystemExit(f'1.9.0 missing patch site {name}')
    address = sites[name]
    offset = address - gen.base
    # Exercise every bit of the instruction, and the preceding words checked by camera patches.
    mutations = [(offset, 0)]
    for pos in range(offset - 16 if name.startswith('cam_pitch_') else offset, offset + 4):
        for bit in range(8):
            mutations.append((pos, 1 << bit))
    for pos, mask in mutations:
        image[pos] ^= mask
        try:
            result = 'ok\t' + patch(gen, address)
        except (AssertionError, StopIteration, struct.error):
            result = 'invalid'
        image[pos] ^= mask
        commands.append(f'{name} {address:x} {pos:x} {mask:x}')
        expected.append((name, pos, mask, result))
process = subprocess.run(['build/s9-cpp/patches_check', str(image_path)],
                         input='\n'.join(commands) + '\n', text=True, capture_output=True, check=True)
actual = process.stdout.splitlines()
if len(actual) != len(expected):
    raise SystemExit(f'expected {len(expected)} results, got {len(actual)}: {process.stderr}')
for (name, pos, mask, want), got in zip(expected, actual):
    if want != got:
        raise SystemExit(f'{name} offset {pos:#x} xor {mask:#x}\nPython: {want}\nC++: {got}')
print(f'{version}: {len(patches.PATCHES)} sites, {len(expected)} validation/output cases identical')
