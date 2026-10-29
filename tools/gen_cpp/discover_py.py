#!/usr/bin/env python3
"""Canonical output of the production generator's discovery pass."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from gen import Module, discover
from layout import read

if len(sys.argv) != 4:
    raise SystemExit('usage: discover_py.py EXEFS LAYOUT MODULE')
exefs, layout_path, name = sys.argv[1:]
base = next(base for module, base, _, _ in read(layout_path) if module == name)
m = Module(name, os.path.join(exefs, name), base, 'c')
entries, rounds = discover(m, m.boundaries())
print(f'rounds {rounds}')
for a, (end, labels, tables) in sorted(entries.items()):
    print(f'function {a:x} {end:x} ' + ','.join(f'{x:x}' for x in sorted(labels)))
    for pc, targets in sorted(tables.items()):
        value = 'all' if targets == 'all' else ','.join(f'{x:x}' for x in targets)
        print(f'table {pc:x} {value}')
for name in ('unsupported', 'jumptables', 'br_all_labels', 'br_dispatch'):
    print(f'stat {name} {m.stats[name]}')
