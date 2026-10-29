#!/usr/bin/env python3
"""Canonical slice-1 reference, using the production Python generator's parsers."""
import argparse
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import layout
from nso import Nso, header
from analyze import eh_frame_functions, eh_frame_extents
from gen import Module

p = argparse.ArgumentParser()
p.add_argument('exefs')
p.add_argument('layout')
p.add_argument('module')
a = p.parse_args()
computed = layout.checked(a.exefs)
expected = layout.read(a.layout)
if computed != expected:
    raise SystemExit('layout mismatch')
for name, base, size, build_id in computed:
    print(f'layout {name} {base:x} {size:x} {build_id}')
base = next(base for name, base, _, _ in computed if name == a.module)
path = os.path.join(a.exefs, a.module)
if header(path)['build_id'] != dict((name, bid) for name, _, _, bid in expected)[a.module]:
    raise SystemExit('build id mismatch')
n = Nso(path)
m = Module(a.module, path, base, 'c')
def hxstr(s): return s.encode('utf-8').hex()
def fnv(data):
    h = 14695981039346656037
    for x in data:
        h = ((h ^ x) * 1099511628211) & ((1 << 64) - 1)
    return h
print(f'module {a.module} {base:x} {n.size:x} {n.bss_size:x}')
for name in ('text', 'rodata', 'data'):
    off, size = getattr(n, name)
    print(f'segment {name} {off:x} {size:x} {fnv(n.image[off:off+size]):016x}')
print(f'mod0 {n.dynamic:x} {n.bss_start:x} {n.bss_end:x} {n.eh_start:x} {n.eh_end:x} {n.module_obj:x}')
for tag, val in sorted(n.dyn.items()): print(f'dynamic {tag:x} {val:x}')
for needed in n.needed: print(f'needed {hxstr(needed)}')
for name, info, shndx, value, size in n.syms:
    print(f'symbol {hxstr(name) or "-"} {info:x} {shndx:x} {value:x} {size:x}')
for off, typ, sym, add in n.relocs: print(f'reloc {off:x} {typ:x} {sym:x} {add & ((1<<64)-1):x}')
for addr in eh_frame_functions(n): print(f'eh {addr:x}')
for start, end in eh_frame_extents(n): print(f'fde {start:x} {end:x}')
for addr in m.boundaries(): print(f'boundary {addr:x}')
