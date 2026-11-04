#!/usr/bin/env python3
"""Compare native and Python generator output on synthetic NSOs (no game files)."""
import filecmp
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import lz4.block

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from test_layout import FUNCS_AT, module, nso_file


def exercise_flags(data, build_id):
    segments = []
    for i in range(3):
        offset, _, size = struct.unpack_from('<III', data, 0x10 + 16 * i)
        packed, = struct.unpack_from('<I', data, 0x60 + 4 * i)
        segments.append(bytearray(lz4.block.decompress(data[offset:offset + packed], uncompressed_size=size)))
    # Compare a mix of flag definitions, reads, loops and calls. Every pair is an exported function.
    words = [0xF100041F, 0x54000000,  # cmp x0,#1; b.eq to itself
             0xB1000421, 0xD65F03C0,  # adds x1,x1,#1; ret
             0x9A020020, 0xD65F03C0,  # adc x0,x1,x2; ret
             0xEB02003F, 0xD65F03C0,  # cmp x1,x2; ret
             0xD2800020, 0xD65F03C0,  # mov x0,#1; ret
             0xF100043F, 0x54FFFFE1,  # cmp x1,#1; b.ne backward
             0xD2800060, 0xD65F03C0,
             0x97FFFFF2, 0xD65F03C0]
    struct.pack_into('<' + 'I' * len(words), segments[0], FUNCS_AT, *words)
    return nso_file(segments, 0, build_id)


def main():
    native = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/s9-cpp/gen'
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        exefs = root / 'exefs'
        exefs.mkdir()
        for name, build_id in [('main', 1), ('subsdk0', 2), ('sdk', 3)]:
            data, _, _ = module(name, 4096, 4096, 0, 8, [], build_id)
            (exefs / name).write_bytes(exercise_flags(data, build_id))
        for options in ([], ['--locals'], ['--locals', '--reg-locals', '3'],
                        ['--locals', '--no-callee-saved-trust']):
            py, cpp = root / 'py', root / 'cpp'
            common = ['all', str(exefs)]
            suffix = ['--no-sites', '--simd', 'c'] + options
            subprocess.run([sys.executable, str(ROOT / 'tools/gen.py'), *common, str(py), *suffix], check=True)
            subprocess.run([str(native), *common, str(cpp), *suffix], check=True)
            left = {p.relative_to(py) for p in py.rglob('*') if p.is_file()}
            right = {p.relative_to(cpp) for p in cpp.rglob('*') if p.is_file()}
            if left != right:
                raise AssertionError(f'file lists differ: {left ^ right}')
            for path in sorted(left):
                if not filecmp.cmp(py / path, cpp / path, shallow=False):
                    raise AssertionError(f'output differs with {options}: {path}')
            print(f'parity {options}: {len(left)} files identical', flush=True)


if __name__ == '__main__':
    main()
