#!/usr/bin/env bash
# Host-only M12 pipeline test. Game-derived objects and cache stay under build/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
llvm="$root/build/ps5/llvm"
src="$llvm/llvm-project-18.1.8.src"
host=${LLVM_HOST_BUILD:-"$llvm/host"}
deps="$root/build/s9-cpp/deps"
input=${GAMECODE_EXEFS:-"$root/build/exefs-1.9.0"}
include=${GAMECODE_INCLUDE_ROOT:-"$root/build/ps5/s8-input"}
archive="$root/build/ps5/gamecode/libgamecode.a"
[[ $($host/bin/llvm-config --version) == 18.1.8 ]] || { echo 'LLVM 18.1.8 host tree required' >&2; exit 1; }
[[ -f $archive && -f $deps/capstone-build/libcapstone.a ]] || { echo 'archive and host capstone required' >&2; exit 1; }
mkdir -p build
dir=$(mktemp -d "$root/build/gamecode-compile-test.XXXXXX")
mkdir -p "$dir/reference" "$dir/cache"
llvm-ar-18 t "$archive" > "$dir/archive.list"
(cd "$dir/reference" && llvm-ar-18 x "$archive")
mkdir -p "$dir/include-root/runtime"
cp runtime/cpu.h runtime/simd*.h runtime/gamelib.h "$dir/include-root/runtime/"
ln -s "$include/include" "$dir/include-root/include"
ln -s "$include/clang-include" "$dir/include-root/clang-include"
clang++-18 -std=c++20 -O2 -fuse-ld=lld -I"$deps/capstone-5.0.9/src/include" \
    -I"$src/clang/include" -I"$host/tools/clang/include" -I"$src/llvm/include" -I"$host/include" \
    tools/test_gamecode_compile.cpp runtime/gamecode_compile.cpp tools/gen_cpp/patches.cpp \
    "$deps/capstone-build/libcapstone.a" -Wl,--start-group "$host"/lib/libclang*.a "$host"/lib/libLLVM*.a \
    -Wl,--end-group $($host/bin/llvm-config --link-static --system-libs support) -pthread -o "$dir/test"
"$dir/test" "$input" "$dir/include-root" "$dir/cache" "$dir/reference" "$dir/archive.list" | tee "$dir/cold.log"
"$dir/test" "$input" "$dir/include-root" "$dir/cache" "$dir/reference" "$dir/archive.list" | tee "$dir/warm.log"
if ! grep -q 'objects=996 .*hits=0 misses=996 stores=996 ' "$dir/cold.log"; then
    echo 'cold run did not compile every object' >&2
    exit 1
fi
if ! grep -q 'objects=996 .*hits=996 misses=0 stores=0 ' "$dir/warm.log"; then
    echo 'warm run did not hit every object' >&2
    exit 1
fi
printf 'Results and private cache: %s\n' "$dir"
