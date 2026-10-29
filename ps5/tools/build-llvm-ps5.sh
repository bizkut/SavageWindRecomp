#!/usr/bin/env bash
# Build the LLVM 18 C frontend and X86 backend for the PS5 title.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
out="$root/build/ps5/llvm"
version=18.1.8
archive="$out/llvm-project-$version.src.tar.xz"
source="$out/llvm-project-$version.src"
checksum=0b58557a6d32ceee97c8d533a59b9212d87e0fc4d2833924eb6c611247db2f2a
mkdir -p "$out"
if [[ ! -f $archive ]]; then
    url="https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/llvm-project-$version.src.tar.xz"
    curl -fL --retry 2 -o "$archive.tmp" "$url"
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --strict
if [[ ! -d $source ]]; then tar -xf "$archive" -C "$out"; fi
flags="-target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default -isysroot $sdk"
flags+=" -isystem $sdk/target/include/c++/v1 -isystem $sdk/target/include"
flags+=" -fno-stack-protector -fno-plt -femulated-tls"
# The PS5 ABI does not propagate alignment from an empty base class.
if patch --dry-run --forward -s -p1 -d "$source" < "$root/ps5/patches/llvm-18-ps5-smallvector.patch"; then
    patch --forward -s -p1 -d "$source" < "$root/ps5/patches/llvm-18-ps5-smallvector.patch"
elif ! patch --dry-run --reverse -s -p1 -d "$source" < "$root/ps5/patches/llvm-18-ps5-smallvector.patch"; then
    echo 'LLVM alignment patch does not match source' >&2; exit 1
fi
if patch --dry-run --forward -s -p1 -d "$source" < "$root/ps5/patches/llvm-18-ps5-trailingobjects.patch"; then
    patch --forward -s -p1 -d "$source" < "$root/ps5/patches/llvm-18-ps5-trailingobjects.patch"
elif ! patch --dry-run --reverse -s -p1 -d "$source" < "$root/ps5/patches/llvm-18-ps5-trailingobjects.patch"; then
    echo 'LLVM trailing objects patch does not match source' >&2; exit 1
fi
host=${LLVM_HOST_BUILD:-"$out/host"}
# LLVM_APPEND_VC_REV=OFF: the compiler's identification string (.comment in every object) would otherwise carry
# this repository's git commit, so the same LLVM source built at two commits made objects that differed in 38 bytes.
cmake -S "$source/llvm" -B "$host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=clang -DLLVM_APPEND_VC_REV=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF
cmake --build "$host" --parallel "${LLVM_JOBS:-$(nproc)}" \
    --target llvm-tblgen clang-tblgen clangFrontend clangCodeGen clangDriver LLVMX86CodeGen LLVMX86AsmParser llvm-config
cmake -S "$source/llvm" -B "$out/target" -G Ninja \
    -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DLLVM_TABLEGEN="$host/bin/llvm-tblgen" -DCLANG_TABLEGEN="$host/bin/clang-tblgen" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_AR="$sdk/bin/llvm-ar" -DCMAKE_RANLIB="$sdk/bin/llvm-ranlib" \
    -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=clang -DLLVM_APPEND_VC_REV=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_BUILD_TOOLS=OFF -DCLANG_BUILD_TOOLS=OFF
# Compile real LLVM/clang trailing-object layouts for the console ABI.
# shellcheck disable=SC2086
clang++-18 $flags -std=c++17 -fno-access-control -fsyntax-only \
    -I"$source/llvm/include" -I"$out/target/include" -I"$source/clang/include" -I"$out/target/tools/clang/include" \
    "$root/ps5/tools/llvm-ps5-layout-check.cpp"
start=$SECONDS
cmake --build "$out/target" --parallel "${LLVM_JOBS:-$(nproc)}" \
    --target clangFrontend clangCodeGen clangDriver LLVMX86CodeGen LLVMX86AsmParser
printf 'LLVM build elapsed %d seconds\n' "$((SECONDS-start))" >&2
du -ch "$out"/target/lib/*.a | tail -1 >&2
