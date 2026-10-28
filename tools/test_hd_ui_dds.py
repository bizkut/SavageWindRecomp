#!/usr/bin/env python3
"""Exercise the console DDS encoder using a host build of pinned bc7enc_rdo."""
import argparse
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
from PIL import Image
import hd_ui_pack as pc


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bc7enc_source', type=Path, help='directory containing bc7enc.h and bc7enc.cpp')
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='hdui-dds-') as temp:
        library = Path(temp) / 'dds.so'
        subprocess.run(['c++', '-shared', '-fPIC', '-O2', '-std=c++17', '-I'+str(args.bc7enc_source),
                        str(root/'runtime/nvn/hd_ui_dds.cpp'), str(args.bc7enc_source/'bc7enc.cpp'), '-o', str(library)], check=True)
        lib = ctypes.CDLL(str(library))
        lib.hd_ui_dds_encode.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
        libc = ctypes.CDLL(None)
        libc.free.argtypes = [ctypes.c_void_p]
        encoder = Path(temp) / 'bc7'
        subprocess.run(['c++', '-O2', '-std=c++17', '-I'+str(args.bc7enc_source),
                        str(root/'tools/hd_ui_bc7.cpp'), str(args.bc7enc_source/'bc7enc.cpp'), '-o', str(encoder)], check=True)
        for w,h in ((1,1),(2,1),(7,9),(32,16)):
            for channels in (1,2,4):
                for srgb in (0,1):
                    pixels = bytes((i*73+19)%256 for i in range(w*h*channels))
                    result, size = ctypes.c_void_p(), ctypes.c_size_t()
                    assert lib.hd_ui_dds_encode(ctypes.c_char_p(pixels),w,h,channels,srgb,ctypes.byref(result),ctypes.byref(size)) == 0
                    try:
                        blob = ctypes.string_at(result,size.value)
                    finally:
                        libc.free(result)
                    expected_size, levels, mw, mh = 148, 0, w, h
                    while True:
                        expected_size += ((mw+3)//4)*((mh+3)//4)*(8 if channels == 1 else 16)
                        levels += 1
                        if mw == mh == 1: break
                        mw,mh=max(1,mw//2),max(1,mh//2)
                    assert len(blob) == expected_size and blob[:4] == b'DDS '
                    assert (u32(blob,12),u32(blob,16),u32(blob,28)) == (h,w,levels)
                    assert blob[84:88] == b'DX10' and u32(blob,132) == 3 and u32(blob,140) == 1
                    assert u32(blob,128) == (80 if channels == 1 else 83 if channels == 2 else 99 if srgb else 98)
                    if channels == 4:
                        reference = subprocess.run([str(encoder), str(w), str(h)], input=pixels,
                                                   stdout=subprocess.PIPE, check=True).stdout
                        assert blob[148:148+len(reference)] == reference
                    else:
                        planes = [Image.frombytes('L', (w,h), pixels[c::channels]) for c in range(channels)]
                        reference = pc.dds(planes)
                        top = ((w+3)//4)*((h+3)//4)*(8 if channels == 1 else 16)
                        assert blob[148:148+top] == reference[148:148+top]
        print('DDS BC4/BC5/BC7 headers and complete mip sizes passed')


if __name__ == '__main__':
    main()
