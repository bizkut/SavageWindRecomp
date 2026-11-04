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
def compare(active_sites, names, label):
    gen = SimpleNamespace(nso=SimpleNamespace(image=image), base=game_sites.MAIN_BASE, sites=active_sites)
    commands = [str(len(active_sites))] + [f'{name} {value:x}' for name, value in active_sites.items()]
    expected = []
    for name in names:
        if name not in active_sites:
            continue
        patch = patches.PATCHES[name]
        address = active_sites[name]
        offset = address - gen.base
        # Exercise instruction bits, camera predecessors, and the separate call checked by no_wear_hit.
        offsets = range(offset - 16 if name.startswith('cam_pitch_') else offset, offset + 4)
        positions = list(offsets)
        if name == 'no_wear_hit' and 'link_life_store' in active_sites:
            call = active_sites['link_life_store'] - gen.base - 0x20
            positions.extend(range(call, call + 4))
        mutations = [(offset, 0)] + [(pos, 1 << bit) for pos in positions for bit in range(8)]
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
        raise SystemExit(f'{label}: expected {len(expected)} results, got {len(actual)}: {process.stderr}')
    for (name, pos, mask, want), got in zip(expected, actual):
        if want != got:
            raise SystemExit(f'{label}: {name} offset {pos:#x} xor {mask:#x}\nPython: {want}\nC++: {got}')
    print(f'{version} {label}: {len(expected)} validation/output cases identical')


missing = patches.PATCHES.keys() - sites.keys()
if missing:
    raise SystemExit(f'1.9.0 missing patch sites: {sorted(missing)}')
compare(sites, patches.PATCHES, 'all sites')
without_link_life = dict(sites)
without_link_life.pop('link_life_store', None)
compare(without_link_life, ['no_wear_hit'], 'without optional link life site')
