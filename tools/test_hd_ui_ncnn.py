#!/usr/bin/env python3
"""Compare the tiled ncnn wrapper against the pinned Real-ESRGAN CLI.

Supply a host-built hd_ui_ncnn shared library, the extracted release directory,
then one or more user-owned RGB PNG textures. Both scales are checked.
"""
import argparse
import ctypes
import math
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageStat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('library', type=Path)
    ap.add_argument('release', type=Path)
    ap.add_argument('images', type=Path, nargs='+')
    args = ap.parse_args()
    lib = ctypes.CDLL(str(args.library.resolve()))
    lib.hd_ui_ncnn_create.argtypes = [ctypes.c_char_p] * 3
    lib.hd_ui_ncnn_create.restype = ctypes.c_void_p
    lib.hd_ui_ncnn_destroy.argtypes = [ctypes.c_void_p]
    lib.hd_ui_ncnn_upscale.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.c_uint32, ctypes.c_uint, ctypes.c_void_p]
    models = args.release / 'models'
    model = models / 'realesr-animevideov3-x4.bin'
    u = lib.hd_ui_ncnn_create(os.fsencode(models / 'realesr-animevideov3-x2.param'),
                              os.fsencode(models / 'realesr-animevideov3-x4.param'), os.fsencode(model))
    if not u:
        raise RuntimeError('could not initialize ncnn')
    try:
        with tempfile.TemporaryDirectory(prefix='hdui-parity-') as temp:
            src, ref = Path(temp) / 'in.png', Path(temp) / 'ref.png'
            for path in args.images:
                image = Image.open(path).convert('RGB')
                w, h = image.size
                image.save(src)
                pixels = ctypes.create_string_buffer(image.tobytes())
                for scale in (2, 4):
                    subprocess.run([str(args.release / 'realesrgan-ncnn-vulkan'), '-i', str(src), '-o', str(ref),
                                    '-n', 'realesr-animevideov3', '-m', str(models), '-s', str(scale), '-t', '200'],
                                   check=True)
                    output = ctypes.create_string_buffer(w * h * scale * scale * 3)
                    if lib.hd_ui_ncnn_upscale(u, pixels, w, h, scale, output):
                        raise RuntimeError(f'ncnn inference failed for {path} scale {scale}')
                    actual = Image.frombytes('RGB', (w * scale, h * scale), output.raw)
                    expected = Image.open(ref).convert('RGB')
                    if expected.size != actual.size:
                        raise AssertionError('reference dimensions differ')
                    rms = ImageStat.Stat(ImageChops.difference(actual, expected)).rms
                    mse = sum(v * v for v in rms) / 3
                    psnr = float('inf') if mse == 0 else 10 * math.log10(255 * 255 / mse)
                    print(f'{path} x{scale}: PSNR {psnr:.2f} dB')
                    if psnr < 45:
                        raise AssertionError('PSNR below 45 dB')
    finally:
        lib.hd_ui_ncnn_destroy(u)


if __name__ == '__main__':
    main()
