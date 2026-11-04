#!/usr/bin/env bash
# Link only on the user's PC, after generating and compiling their game code.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
sdk="$root/sdk"
export PS5_PAYLOAD_SDK="$sdk"
[[ $# -ge 2 ]] || { echo "usage: $0 prod.keys package..." >&2; exit 2; }
keys=$1; shift
[[ ! -e "$root/work/exefs" && ! -e "$root/generated-ps5" && ! -e "$root/build/ps5/gamecode" ]] || {
    echo "Existing game build found. Use a fresh unpacked kit to avoid mixing game versions." >&2; exit 2;
}
mkdir -p work/exefs
"$root/bin/savagewind-exefs" --keys "$keys" --out "$root/work/exefs" "$@"
python3 tools/gen.py all "$root/work/exefs" "$root/generated-ps5" --hle runtime/hle/hle_symbols.txt
bash ps5/tools/build-gamecode.sh
build="$root/work/link"
mkdir -p "$build"
source "$root/ps5/tools/link-recipe.sh"
link_recipe "$sdk" "$build" "$root/lib/libvulkan_radeon.ps5.a"
mapfile -t names < "$root/objects/runtime-objects.list"
objects=()
for name in "${names[@]}"; do
    [[ $name != */* && $name == *.o && -f $root/objects/$name ]] || { echo "invalid runtime object: $name" >&2; exit 2; }
    objects+=("$root/objects/$name")
done
native="$root/ps5/tooling/native"
agc="$root/objects/libSceAgc.so"
agc_driver="$root/objects/libSceAgcDriver.so"
"$sdk/bin/prospero-lld" -T "$native/ps5-pie-unwind.ld" -L "$native" --eh-frame-hdr --error-limit=0 \
    --Map="$build/title.map" --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    "${link_flags[@]}" -e _start -o "$build/llvm-pie.elf" \
    "$root/objects/app_crt.o" "$root/objects/app_cpp_runtime.o" "${objects[@]}" \
    --gc-sections --wrap=exit --wrap=_exit --wrap=_Exit --wrap=quick_exit --wrap=abort \
    --wrap=sceSystemServiceLoadExec --wrap=ps5_heap_malloc --wrap=ps5_heap_calloc \
    --wrap=ps5_heap_memalign --wrap=ps5_heap_free --start-group \
    "$root/build/ps5/gamecode/libgamecode.a" "$root/lib/libsavagewind-package.a" "$root/lib/libncnn.a" \
    "$root/lib/libpietendo.a" "$root/lib/libtoolchain.a" "$root/lib/libfmt.a" "$root/lib/liblz4.a" \
    "$root/lib/libmbedtls.a" "$root/lib/libshaderc_combined.a" \
    "$root/lib/libavformat.a" "$root/lib/libavcodec.a" "$root/lib/libswscale.a" \
    "$root/lib/libswresample.a" "$root/lib/libavutil.a" "$root/lib/libssl.a" "$root/lib/libcrypto.a" \
    "$root/lib/libbz2.a" "$root/lib/liblzma.a" "$root/lib/libiconv.a" --end-group \
    "${link_inputs[@]}" "$agc" "$agc_driver" --as-needed "${link_stubs[@]}"
python3 "$root/ps5/tools/check-title-imports.py" "$build/llvm-pie.elf" \
    "$native/runtime/api-surface.txt" "$agc" "$agc_driver" "${link_stubs[@]}"
"$root/bin/ps5-native-tool" link --in "$build/llvm-pie.elf" --out "$build/eboot.elf" \
    --stub-dir "$sdk/target/lib" --stub "$agc" --stub "$agc_driver" --stub "$build/stubs/libSceIme.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
mkdir -p "$root/title/PPSA90212"
"$root/bin/ps5-native-tool" self --sign --in "$build/eboot.elf" --out "$root/title/PPSA90212/eboot.bin" --magic 0x1D3D154F
"$root/bin/ps5-native-tool" self --inspect --file "$root/title/PPSA90212/eboot.bin" >/dev/null
echo "Built $root/title/PPSA90212/eboot.bin; keep it private."
