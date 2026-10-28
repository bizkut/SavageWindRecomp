#!/usr/bin/env python3
"""Compare console layout extraction with the PC generator on user-owned .pack files."""
import argparse
import ctypes
from pathlib import Path
import subprocess
import tempfile

from PIL import Image
import hd_ui_pack as pc


class Texture(ctypes.Structure):
    _fields_ = [('name', ctypes.c_void_p), ('name_length', ctypes.c_size_t),
                ('format', ctypes.c_uint32), ('width', ctypes.c_uint32), ('height', ctypes.c_uint32),
                ('channels', ctypes.c_uint32), ('scale', ctypes.c_uint32), ('hash', ctypes.c_uint64),
                ('linear', ctypes.c_void_p), ('linear_size', ctypes.c_size_t), ('pixels', ctypes.c_void_p)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('packs', type=Path, nargs='+')
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    formats = {0x0201:(1,1,1), 0x0701:(1,2,2), 0x0901:(1,2,2), 0x0b01:(1,4,4), 0x0b06:(1,4,4),
               0x1a01:(4,8,4), 0x1a06:(4,8,4), 0x1c01:(4,16,4), 0x1c06:(4,16,4),
               0x1d01:(4,8,1), 0x1e01:(4,16,2)}
    with tempfile.TemporaryDirectory(prefix='hd-ui-extract-test-') as tmp:
        tmp = Path(tmp)
        pc.setup_policy(tmp)
        library = tmp / 'extract.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', *[str(root / 'runtime/nvn' / p) for p in
                        ('hd_ui_extract.c', 'hd_ui_policy.c', 'tegra.c', 'formats.c')], '-o', str(library)], check=True)
        lib = ctypes.CDLL(str(library))
        callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(Texture))
        lib.hd_ui_extract_layout.argtypes = [ctypes.c_void_p, ctypes.c_size_t, callback_type, ctypes.c_void_p]
        count = 0
        for pack in args.packs:
            for layout, compressed in pc.sarc(pack.read_bytes(), layouts=True):
                archive = pc.yaz(compressed)
                actual = []
                @callback_type
                def collect(_, ptr):
                    t = ptr.contents
                    actual.append((ctypes.string_at(t.name, t.name_length).decode(), t.format, t.width, t.height,
                                   t.channels, t.scale, t.hash, ctypes.string_at(t.linear, t.linear_size),
                                   ctypes.string_at(t.pixels, t.width*t.height*t.channels)))
                    return 0
                assert lib.hd_ui_extract_layout(ctypes.c_char_p(archive), len(archive), collect, None) == 0
                expected = []
                for name, b in pc.sarc(archive):
                    if not name.lower().endswith('.bntx'):
                        continue
                    for i in range(pc.u32(b, 0x24)):
                        off = pc.u64(b, pc.u64(b, 0x28) + 8*i)
                        np = pc.u64(b, off+0x60)
                        entry = b[np+2:np+2+pc.u16(b,np)].decode()
                        fmt, w, h = pc.u32(b,off+0x1c), pc.u32(b,off+0x24), pc.u32(b,off+0x28)
                        block, bpb, channels = formats[fmt]
                        if pc.policy.hd_ui_texture_policy(w,h,1024,channels,None,0) in (1,2):
                            continue
                        data_at = pc.u64(b, pc.u64(b,off+0x70))
                        linear = pc.deswizzle(b[data_at:],w,h,bpb,pc.u32(b,off+0x34)&7,block)
                        if fmt in (0x1d01,0x1e01):
                            planes = pc.unpack(linear,w,h,channels)
                        elif channels < 4:
                            planes = [Image.frombytes('L',(w,h),linear[c::channels]) for c in range(channels)]
                        else:
                            planes = list(pc.unpack_colour(linear,w,h,fmt).split())
                        if pc.texture_policy(planes,1024):
                            continue
                        pixels = (planes[0] if channels == 1 else Image.merge({2:'LA',4:'RGBA'}[channels],planes)).tobytes()
                        hash_value = 0xcbf29ce484222325
                        for value in linear:
                            hash_value = ((hash_value ^ value) * 0x100000001b3) & 0xffffffffffffffff
                        expected.append((entry,fmt,w,h,channels,pc.policy.hd_ui_texture_scale(w,h),hash_value,linear,pixels))
                assert actual == expected, f'extraction differs in {pack}:{layout}'
                count += len(actual)
        print(f'{count} selected texture entries match PC extraction')


if __name__ == '__main__':
    main()
