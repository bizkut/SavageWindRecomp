#!/usr/bin/env python3
"""Create a console-only ZIP from explicitly selected inputs and inspect its actual members."""
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / 'build/ps5/variants/gamecode-console/PPSA90212'
SDK = ROOT / '.deps/ps5/ps5-payload-sdk/target/include'
TAG, COMMIT, candidate = sys.argv[1:]
CANDIDATE = candidate == '1'
OUT = ROOT / 'build/ps5/releases' / f'SavageWind-PS5-{TAG}.zip'


def digest(data):
    return hashlib.sha256(data).hexdigest()

def check_font_input(name, data):
    parts = tuple(part.lower() for part in Path(name).parts)
    suffix = Path(name).suffix.lower()
    if (any(part in ('glyphs', 'glyph-cache', 'font-cache', 'fonts-cache') for part in parts) or
            ('cache' in parts and any(part.startswith(('glyph', 'font')) for part in parts))):
        raise SystemExit(f'forbidden font cache: {name}')
    if suffix in ('.otf', '.ttf', '.ttc', '.otc') or suffix.startswith('.woff'):
        allowed = {
            'PPSA90212/FiraSans-SemiBold.ttf': 'db0321f83eb3e9f527b8af384a1b3fefdc1039cf2b06fd39b3f61492bda9561c',
            'PPSA90212/fonts/OpenSans.ttf': '36643644f318a812aab2d2ed3bb98f8cf0872527f835fe9398d95fe6b9adb878',
        }
        if digest(data) != allowed.get(name):
            raise SystemExit(f'unapproved font: {name}')



def contents(path):
    if not path.is_file() or path.is_symlink():
        raise SystemExit(f'missing or unsafe input: {path}')
    return path.read_bytes()


# Fixed title files, not a copy of a possibly contaminated build directory.
title_files = (
    'eboot.bin', 'storagebroker.elf', 'sce_module/libc.prx', 'sce_sys/param.json', 'sce_sys/icon0.png',
    'home-art-original-icon0.png', 'FiraSans-SemiBold.ttf', 'FiraSans-OFL.txt',
    'fonts/OpenSans.ttf', 'notices/OPENSANS.txt', 'notices/FREETYPE.txt',
    'network.fgw', 'MihawkGen-LICENSE.md', 'MihawkGen-THIRD_PARTY_NOTICES.txt',
    'FidelityFX-LICENSE.txt', 'realesr-animevideov3-x2.param', 'realesr-animevideov3-x4.param',
    'realesr-animevideov3-x4.bin', 'REAL-ESRGAN-LICENSE.txt', 'NCNN-LICENSE.txt',
    'BC7ENC-LICENSE.txt', 'LLVM-LICENSE.txt', 'Capstone-LICENSE.txt', 'Capstone-LLVM-LICENSE.txt',
)
files = {f'PPSA90212/{name}': contents(APP / name) for name in title_files}
lock = {}
for line in (ROOT / 'ps5/deps.lock').read_text().splitlines():
    parts = line.split()
    if parts and not parts[0].startswith('#'):
        lock[parts[0]] = parts[-1]
if 'sha256:' + digest(files['PPSA90212/network.fgw']) != lock['mihawkgen-network']:
    raise SystemExit('MihawkGen weights differ from the pinned digest')
model_zip = ROOT / 'build/ps5/hd-ui-deps/model.zip'
if 'sha256:' + digest(contents(model_zip)) != lock['realesrgan-ubuntu']:
    raise SystemExit('Real-ESRGAN model archive differs from the pinned digest')
with zipfile.ZipFile(model_zip) as models:
    for model in ('realesr-animevideov3-x2.param', 'realesr-animevideov3-x4.param',
                  'realesr-animevideov3-x4.bin'):
        if files[f'PPSA90212/{model}'] != models.read(f'models/{model}'):
            raise SystemExit(f'model differs from the pinned archive: {model}')
symbols = subprocess.check_output(
    ['llvm-nm-18', '--defined-only', str(ROOT / 'build/ps5/gamecode-console/llvm-pie.elf')])
game_symbols = re.compile(
    rb'\b(?:f_70[0-9a-f]{8,}|(?:main|sdk|subsdk[0-9]+)_fn_(?:addrs|ptrs|count)|swgame_tables)\b', re.I)
if game_symbols.search(symbols):
    raise SystemExit('game-derived symbols found in the console title link image')
print('Console link image: no known generated game-code symbols')
files['storagebroker.elf'] = contents(ROOT / 'build/ps5/storagebroker/storagebroker.elf')
if files['PPSA90212/storagebroker.elf'] != files['storagebroker.elf']:
    raise SystemExit('title storage broker differs from release storage broker')
files['README.txt'] = contents(ROOT / 'README-PS5.md')
files['LICENSE'] = contents(ROOT / 'LICENSE')
source = contents(ROOT / 'ps5/release/SOURCE.txt').decode('utf-8')
if '@COMMIT@' not in source or '@TAG@' not in source:
    raise SystemExit('SOURCE.txt must identify the exact commit and tag')
if not CANDIDATE and 'UNRESOLVED' in source:
    raise SystemExit('SOURCE.txt contains unresolved release blockers; public packaging is prohibited')
source = source.replace('@COMMIT@', COMMIT).replace('@TAG@', TAG)
if CANDIDATE:
    source = ('UNRELEASED CANDIDATE — not for distribution\n'
              'This build uses uncommitted changes; the commit below is its base, not corresponding source.\n\n'
              + source)
files['SOURCE.txt'] = source.encode()

# Licence texts are selected by name; a new notice requires an explicit allowlist change.
notice_root = ROOT / 'ps5/release/notices'
notices = (
    'BC7ENC.txt', 'CAPSTONE.txt', 'CAPSTONE-LLVM.txt', 'FFMPEG-LGPL3.txt', 'FIDELITYFX.txt',
    'FIRASANS.txt', 'FMT.txt', 'FREETYPE.txt', 'GLSLANG.txt', 'IMGUI.txt', 'LIBPIETENDO.txt',
    'LIBTOOLCHAIN.txt', 'LLVM.txt', 'LZ4.txt', 'MBEDTLS.txt', 'MBEDTLS-APACHE.txt', 'MESA.txt',
    'MESA-LINUX-SYSCALL.txt', 'MIHAWKGEN.txt', 'MIHAWKGEN-THIRD-PARTY.txt', 'NCNN.txt',
    'NLOHMANN-JSON.txt', 'NSTOOL.txt', 'OPENSANS.txt', 'PS5-PAYLOAD-SDK.txt',
    'REAL-ESRGAN.txt', 'SAVAGE-WIND.txt', 'SHADERC.txt', 'SMAA.txt', 'SPIRV-HEADERS.txt',
    'SPIRV-TOOLS.txt', 'STB-IMAGE.txt', 'STB-IMAGE-WRITE.txt', 'THIRD_PARTY_NOTICES.txt',
    'VMA.txt', 'ZLIB.txt',
)
for name in notices:
    data = contents(notice_root / name)
    if name == 'THIRD_PARTY_NOTICES.txt':
        data = data.replace(b'@COMMIT@', COMMIT.encode()).replace(b'@TAG@', TAG.encode())
    files[f'notices/{name}'] = data

# These are the exact headers the on-console compiler needs, selected from the SDK and host Clang,
# and checked byte-for-byte against those inputs rather than trusting extra files in the title tree.
clang = Path(subprocess.check_output(['clang-18', '-print-resource-dir'], text=True).strip()) / 'include'
for source_dir, dest in ((SDK, 'include'), (clang, 'clang-include')):
    for path in sorted(source_dir.rglob('*')):
        if path.is_file():
            relative = path.relative_to(source_dir)
            name = f'PPSA90212/gamecode-include/{dest}/{relative.as_posix()}'
            expected = path.read_bytes()  # SDK includes may be symlinks; staged title files may not.
            actual = contents(APP / 'gamecode-include' / dest / relative)
            if expected != actual:
                raise SystemExit(f'compiler header differs from its input: {name}')
            files[name] = actual
for name in ('cpu.h', 'gamelib.h', 'simd.h', 'simd_fp.h', 'simd_softfma.h'):
    data = contents(ROOT / 'runtime' / name)
    dest = f'PPSA90212/gamecode-include/runtime/{name}'
    if data != contents(APP / 'gamecode-include/runtime' / name):
        raise SystemExit(f'compiler header differs from source: {dest}')
    files[dest] = data
files['PPSA90212/gamecode-include/hle_symbols.txt'] = contents(ROOT / 'runtime/hle/hle_symbols.txt')
if files['PPSA90212/gamecode-include/hle_symbols.txt'] != contents(APP / 'gamecode-include/hle_symbols.txt'):
    raise SystemExit('HLE symbols differ from source')

icon = contents(ROOT / 'ps5/sce_sys/icon0.png')
for name in ('PPSA90212/sce_sys/icon0.png', 'PPSA90212/home-art-original-icon0.png'):
    if files[name] != icon:
        raise SystemExit(f'placeholder art digest mismatch: {name}')

# The parser deliberately contains these names and magic constants. Bind the exception to the exact
# built binary digest, print it, and still reject a file that actually begins with a container header.
executables = {name: digest(files[name]) for name in (
    'PPSA90212/eboot.bin', 'PPSA90212/sce_module/libc.prx',
    'PPSA90212/storagebroker.elf', 'storagebroker.elf')}
magic = (b'NSO0', b'Yaz0', b'SARC', b'BNTX', b'FRES', b'NCA0', b'NCA2', b'NCA3')
key_line = re.compile(rb'(?im)^\s*(?:[a-z0-9_]*key[a-z0-9_]*\s*=\s*)?[0-9a-f]{32,64}\s*$')
signature = re.compile(rb'(?<![A-Za-z0-9_])(?:NSO0|Yaz0|SARC|BNTX|FRES|NCA[023])(?![A-Za-z0-9_])')
title = re.compile(rb'breath of the wild', re.IGNORECASE)


def embedded_container(data):
    # SARC includes a fixed header length and byte-order marker. Yaz0 has a big-endian
    # uncompressed length and eight reserved zero bytes. Other formats are checked at
    # file start; literals alone are not evidence of an embedded container.
    for match in re.finditer(b'SARC', data):
        pos = match.start()
        if data[pos + 4:pos + 8] in (b'\x14\x00\xff\xfe', b'\x00\x14\xfe\xff'):
            return 'SARC'
    for match in re.finditer(b'Yaz0', data):
        pos = match.start()
        if len(data) >= pos + 16 and data[pos + 4:pos + 8] != b'\0' * 4 and data[pos + 8:pos + 16] == b'\0' * 8:
            return 'Yaz0'
    return None


for name, data in files.items():
    check_font_input(name, data)
    suffix = Path(name).suffix.lower()
    if suffix in ('.nsp', '.xci', '.nca', '.nsz', '.xcz', '.keys'):
        raise SystemExit(f'forbidden game/key file: {name}')
    if suffix in ('.png', '.dds') and name not in (
            'PPSA90212/sce_sys/icon0.png', 'PPSA90212/home-art-original-icon0.png'):
        raise SystemExit(f'unapproved image: {name}')
    if data[:4] in magic or data[0x200:0x204] in (b'NCA0', b'NCA2', b'NCA3'):
        raise SystemExit(f'container header: {name}')
    if key_line.search(data):
        raise SystemExit(f'key-like hex line: {name}')
    embedded = embedded_container(data)
    if embedded:
        raise SystemExit(f'embedded {embedded} container: {name}')
    if name in executables:
        markers = magic + (b'Breath of the Wild',)
        hits = ', '.join(f'{m.decode()}={data.count(m)}' for m in markers if m in data)
        print(f"Executable literal exception {executables[name]}  {name}  {hits or 'no magic literals'}")
    elif not (name.startswith('notices/') or name in ('README.txt', 'SOURCE.txt', 'LICENSE')):
        if signature.search(data) or title.search(data):
            raise SystemExit(f'game signature in {name}')

# ZIP contains only the explicitly selected files above, with no directory entries or symlinks.
fd, temporary = tempfile.mkstemp(prefix='.release-', suffix='.zip', dir=OUT.parent)
os.close(fd)
try:
    with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6,
                         allowZip64=True) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(temporary) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != set(files):
            raise SystemExit('ZIP members differ from the explicit allowlist')
        for item in archive.infolist():
            if stat.S_ISLNK(item.external_attr >> 16):
                raise SystemExit(f'ZIP symlink: {item.filename}')
            data = archive.read(item)
            if digest(data) != digest(files[item.filename]):
                raise SystemExit(f'ZIP member differs from checked input: {item.filename}')
            print(f'{len(data):12d}  {digest(data)}  {item.filename}')
    os.replace(temporary, OUT)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
print(f'Content check passed: {len(files)} explicitly selected files')
print(f'{OUT.stat().st_size}  {digest(OUT.read_bytes())}  {OUT}')
