#!/usr/bin/env bash
# Build the M12 in-process Clang spike; all game-derived inputs stay in build/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
llvm="$root/build/ps5/llvm"
src="$llvm/llvm-project-18.1.8.src"
target="$llvm/target"
input="$root/build/ps5/s8-input"
[[ -f $target/lib/libclangCodeGen.a ]] || { echo 'build LLVM first with ps5/tools/build-llvm-ps5.sh' >&2; exit 1; }
mkdir -p "$input/runtime" "$root/build/ps5/variants/s8"
cp "$root/generated-ps5/main/main_040.c" "$input/main_040.c"
cp "$root/runtime/"{cpu.h,simd.h,simd_fp.h} "$input/runtime/"
rm -rf -- "$input/include" "$input/clang-include"
cp -aL "$sdk/target/include" "$input/"
cp -aL "$(clang-18 -print-resource-dir)/include" "$input/clang-include"
incs="-I$src/clang/include -I$target/tools/clang/include -I$src/llvm/include -I$target/include"
# Use the same LLVM source release and generated configuration on the host: the
# system's libclang may be a different patch release and produce different code.
host=${LLVM_HOST_BUILD:-"$llvm/host"}
if [[ ! -f $host/CMakeCache.txt && -f $target/NATIVE/CMakeCache.txt ]]; then host="$target/NATIVE"; fi
[[ -f $host/CMakeCache.txt ]] || { echo 'build LLVM first with ps5/tools/build-llvm-ps5.sh' >&2; exit 1; }
cmake --build "$host" --parallel "${LLVM_JOBS:-$(nproc)}" \
    --target clangFrontend clangCodeGen clangDriver LLVMX86CodeGen LLVMX86AsmParser llvm-config
[[ $("$host/bin/llvm-config" --version) == 18.1.8 ]] || { echo 'host LLVM version mismatch' >&2; exit 1; }
host_incs="-I$src/clang/include -I$host/tools/clang/include -I$src/llvm/include -I$host/include"
# Both binaries read the same staged inputs and target PS5.
# shellcheck disable=SC2086
clang++-18 -std=c++17 -O2 -fno-exceptions -fno-rtti -fuse-ld=lld $host_incs \
    "$root/ps5/probe/s8_clang.cpp" -Wl,--start-group "$host"/lib/libclang*.a "$host"/lib/libLLVM*.a \
    -Wl,--end-group $("$host/bin/llvm-config" --link-static --system-libs support) -pthread \
    -o "$root/build/ps5/s8-host"
export PS5_CFLAGS="$incs -fno-exceptions -fno-rtti"
export PS5_LINK_INPUTS="--start-group $target/lib/*.a --end-group --gc-sections"
bash "$root/ps5/tools/build-title.sh" s8 ps5/probe/s8_clang.cpp ps5/probe/s8_compat.c
cp -a "$root/ps5/dist/s8/PPSA90212" "$root/build/ps5/variants/s8/"
printf '%s\n' "Input tree: $input (upload contents to /data/savagewind/s8/)"
