#!/usr/bin/env python3
"""Compare lift dumps, distinguishing missing classes from incorrect implemented lifts."""
import collections
import itertools
import sys

counts = collections.Counter()
examples = []
def category(word):
    op = (word >> 25) & 15
    if op & 14 == 8: return 'immediate'
    if op & 14 == 10: return 'branch/system'
    if op & 5 == 4: return 'load/store'
    if op & 7 == 5: return 'register'
    if op & 7 == 7: return 'fp/simd'
    return 'trap'
with open(sys.argv[1]) as py, open(sys.argv[2]) as cpp:
    for a, b in itertools.zip_longest(py, cpp):
        if a is None or b is None:
            raise SystemExit('different line counts')
        aa, bb = a.rstrip('\n').split('\t'), b.rstrip('\n').split('\t')
        if aa[0] != bb[0]: raise SystemExit('corpus differs')
        group = category(int(aa[0], 16))
        if aa == bb: result = 'identical'
        elif len(bb) == 2 and bb[1] == 'unported': result = 'unported'
        else:
            result = 'mismatch'
            if len(examples) < 12: examples.append((a.rstrip(), b.rstrip()))
        counts[group, result] += 1
for group in ('immediate', 'branch/system', 'load/store', 'register', 'fp/simd', 'trap'):
    if any(counts[group, x] for x in ('identical', 'unported', 'mismatch')):
        print(group, ' '.join(f'{x}={counts[group, x]}' for x in ('identical', 'unported', 'mismatch')))
for a, b in examples: print('PY', a, '\nC++', b)
print('total', ' '.join(f'{x}={sum(v for (g, r), v in counts.items() if r == x)}' for x in ('identical', 'unported', 'mismatch')))
sys.exit(any(r != 'identical' and v for (g, r), v in counts.items()))
