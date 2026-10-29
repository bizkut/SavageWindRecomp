#!/usr/bin/env bash
# Assemble an unpublished PC kit from the current completed PS5 build; never include a linked game.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
build="$root/build/ps5/game"
app="$root/ps5/dist/game/PPSA90212"
[[ -s $build/runtime-objects.list && -x $build/host/ps5-native-tool ]] || {
    echo 'Build the PS5 title first: bash ps5/tools/build-game.sh' >&2; exit 1;
}
sdk=$(bash ps5/tools/setup-deps.sh)
ncnn=$(bash ps5/tools/build-ncnn.sh)
mapfile -t ffmpeg < <(bash ps5/tools/setup-ffmpeg.sh)
[[ ${#ffmpeg[@]} -gt 1 ]] || { echo 'FFmpeg dependency setup failed' >&2; exit 1; }
radv_pin=$(awk '$1 == "radv-archive" { print $3 }' ps5/deps.lock)
[[ "sha256:$(sha256sum .deps/ps5/radv/libvulkan_radeon.ps5.a | cut -d' ' -f1)" == "$radv_pin" ]] || {
    echo 'RADV archive does not match ps5/deps.lock' >&2; exit 1;
}
mkdir -p build/ps5/releases
stage=$(mktemp -d "$root/build/ps5/releases/.stage.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
kit="$stage/savagewind-ps5"
mkdir -p "$kit"/{bin,lib,objects,tools,runtime/hle,ps5/tools,ps5/tooling/native/runtime,sdk/target,title/PPSA90212,notices}
cp README-PS5.md "$kit/"
cp LICENSE "$kit/notices/SavageWind-MIT.txt"
cp ps5/deps.lock "$kit/ps5/"
cp tools/{gen,lifter,analyze,nso,passes,patches,simd_c,layout,game_sites}.py "$kit/tools/"
cp runtime/{cpu,simd,simd_fp,simd_softfma,gamelib,rt,platform}.h "$kit/runtime/"
cp runtime/hle/hle_symbols.txt "$kit/runtime/hle/"
cp ps5/tools/{link-release,setup-release,build-gamecode,link-recipe}.sh ps5/tools/check-title-imports.py "$kit/ps5/tools/"
cp ps5/tooling/prospero-clang18 "$kit/ps5/tooling/"
cp ps5/tooling/native/{ps5-pie.ld,ps5-pie-unwind.ld,app-symbols.map} "$kit/ps5/tooling/native/"
cp ps5/tooling/native/runtime/api-surface.txt "$kit/ps5/tooling/native/runtime/"
cp -RL "$sdk/bin" "$sdk/ldscripts" "$kit/sdk/"
cp -RL "$sdk/target/include" "$sdk/target/lib" "$kit/sdk/target/"
cp "$build/host/ps5-native-tool" "$root/build/ps5/importer/host/clang-build/savagewind-exefs" "$kit/bin/"
cp "$root/build/ps5/storagebroker/storagebroker.elf" "$kit/bin/"
cp "$build/runtime-objects.list" "$kit/objects/"
while IFS= read -r name; do
    [[ $name != */* && $name == *.o && -f $build/obj/$name ]] || { echo "invalid object $name" >&2; exit 1; }
    cp "$build/obj/$name" "$kit/objects/"
done < "$build/runtime-objects.list"
cp "$build/obj/"{app_crt.o,app_cpp_runtime.o} "$build/stubs/"{libSceAgc.so,libSceAgcDriver.so} "$kit/objects/"
cp "$root/.deps/ps5/radv/libvulkan_radeon.ps5.a" "$root/.deps/ps5/shaderc/lib/libshaderc_combined.a" \
   "$root/build/ps5/importer/ps5-build/libsavagewind-package.a" "$ncnn/build/src/libncnn.a" "$kit/lib/"
for name in libpietendo libtoolchain libfmt liblz4 libmbedtls; do
    cp "$root/build/ps5/importer/nstool-source/deps/$name/bin/linux/$name.a" "$kit/lib/"
done
cp "${ffmpeg[@]:1}" "$kit/lib/"
cp -R "$app/sce_sys" "$app/sce_module" "$kit/title/PPSA90212/"
for name in FiraSans-SemiBold.ttf FiraSans-OFL.txt network.fgw realesr-animevideov3-x2.param \
    realesr-animevideov3-x4.param realesr-animevideov3-x4.bin; do cp "$app/$name" "$kit/title/PPSA90212/"; done
for name in MihawkGen-LICENSE.md MihawkGen-THIRD_PARTY_NOTICES.txt FidelityFX-LICENSE.txt \
    REAL-ESRGAN-LICENSE.txt NCNN-LICENSE.txt BC7ENC-LICENSE.txt FiraSans-OFL.txt; do cp "$app/$name" "$kit/notices/"; done
cp /usr/share/common-licenses/LGPL-3 "$kit/notices/FFmpeg-LGPL-3.txt"
cp -R "$root/../forever/PS5_Mesa/licenses" "$kit/notices/Mesa-licenses"
# Check every staged member before creating the archive. Only explicit inputs are copied above.
python3 - "$kit" <<'PY'
import pathlib, re, subprocess, sys
root = pathlib.Path(sys.argv[1])
for path in root.rglob('*'):
    rel = path.relative_to(root)
    low = str(rel).lower()
    if path.is_symlink():
        raise SystemExit(f'refusing symlink: {rel}')
    if not path.is_file():
        continue
    name = path.name.lower()
    parts = {part.lower() for part in rel.parts}
    if (name.endswith(('.nsp', '.xci', '.nca', '.nsz', '.xcz', '.keys')) or
        name in {'eboot.bin', 'eboot.elf', 'llvm-pie.elf', 'layout.c', 'layout.txt', 'main.npdm', 'libgamecode.a'} or
        parts & {'generated', 'generated-ps5', 'exefs', 'romfs', 'hd-ui', 'hd_ui', 'gamecode'} or
        re.fullmatch(r'(main|sdk|subsdk\d+)_(\d+)\.c', name)):
        raise SystemExit(f'refusing game-derived file: {rel}')
    with path.open('rb') as f:
        if f.read(4) == b'NSO0':
            raise SystemExit(f'refusing NSO executable: {rel}')
    if path.suffix in {'.o', '.a'}:
        result = subprocess.run(['llvm-nm-18', '--defined-only', str(path)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        if re.search(rb'\b(?:f_70[0-9a-f]{8,}|(?:main|sdk|subsdk[0-9]+)_fn_(?:addrs|ptrs|count)|swgame_tables)\b',
                     result.stdout, re.IGNORECASE):
            raise SystemExit(f'refusing game-code symbols: {rel}')
PY
zipfile="$root/build/ps5/releases/savagewind-ps5-private.zip"
tmpzip="$stage/release.zip"
(cd "$stage" && zip -qr "$tmpzip" savagewind-ps5)
unzip -l "$tmpzip"
mv -f -- "$tmpzip" "$zipfile"
sha256sum "$zipfile"
du -h "$zipfile"
