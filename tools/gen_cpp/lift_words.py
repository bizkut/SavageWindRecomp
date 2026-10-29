#!/usr/bin/env python3
"""Dump distinct aligned text words and portable-C reference lifts at a fixed PC."""
import argparse
import os
import random
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from nso import Nso
from lifter import Lifter, Unsupported

p = argparse.ArgumentParser()
p.add_argument('nso')
p.add_argument('words')
a = p.parse_args()
if a.nso == 'synthetic':
    rng = random.Random(0x5a19)
    words = sorted({rng.getrandbits(32) for _ in range(100000)})
else:
    n = Nso(a.nso)
    off, size = n.text
    words = sorted({w[0] for w in struct.iter_unpack('<I', n.image[off:off + (size & ~3)])})
lifter = Lifter(0x7000000000, 'c')
with open(a.words, 'w') as out:
    for w in words:
        key = f'{w:08x}'
        out.write(key + '\n')
        try:
            ins = lifter.lift(w, 0x7000000000)
            if ins.kind in ('op', 'trap'):
                value = ins.code
            elif ins.target is not None:
                value = str(ins.target)
                if ins.cond is not None:
                    value += '\t' + ins.cond
            else:
                value = ''
            print(key, ins.kind, value, sep='\t')
        except Unsupported as e:
            print(key, 'unsupported', str(e), sep='\t')
