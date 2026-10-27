#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PC helper step (plan D5): compile the code tools/gen.py generated from the user's ExeFS for the PS5 and archive it
# for the static title link (S3). Flags follow the desktop GENFLAGS; the PS5's Zen 2 is a fixed x86-64-v3 target.
#   bash ps5/tools/build-gamecode.sh [generated-dir]   (default generated-ps5) -> build/ps5/gamecode/libgamecode.a
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
gen=$(realpath -- "${1:-$root/generated-ps5}")
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
out="$root/build/ps5/gamecode"
[[ -f $gen/layout.c ]] || { echo "no generated code in $gen (run tools/gen.py all <exefs> $gen first)" >&2; exit 2; }
mkdir -p "$out/obj"
export PS5_PAYLOAD_SDK="$sdk" root gen out
compile() {
    local src=$1 rel obj
    rel=${src#"$gen"/}
    obj="$out/obj/${rel%.c}.o"
    [[ -f $obj && $obj -nt $src ]] && return 0
    mkdir -p "$(dirname -- "$obj")"
    sh "$root/ps5/tooling/prospero-clang18" -std=gnu11 -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label \
        -fvisibility=hidden -march=x86-64-v3 -fPIC -ffunction-sections -I"$root/runtime" -c "$src" -o "$obj"
}
export -f compile
start=$SECONDS
find "$gen" -name '*.c' -print0 | xargs -0 -n 1 -P "$(nproc)" bash -c 'compile "$0"'
rm -f "$out/libgamecode.a"
find "$out/obj" -name '*.o' -print0 | sort -z | xargs -0 "$sdk/bin/llvm-ar" rcs "$out/libgamecode.a"
echo "compiled $(find "$out/obj" -name '*.o' | wc -l) objects in $((SECONDS - start)) s" >&2
printf '%s\n' "$out/libgamecode.a"
