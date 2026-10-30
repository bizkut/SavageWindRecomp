#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
llvm="$root/build/ps5/llvm"
src="$llvm/llvm-project-18.1.8.src"
host=${LLVM_HOST_BUILD:-"$llvm/host"}
deps="$root/build/s9-cpp/deps"
crypto="$root/build/ps5/importer/host/source/deps/libmbedtls"
dir=$(mktemp -d "$root/build/gamecode-identity-test.XXXXXX")
trap 'rm -rf "$dir"' EXIT
clang-18 -std=c11 -O2 -ffunction-sections -fdata-sections -I"$crypto/include" -c runtime/gamecode_cache.c -o "$dir/cache.o"
clang-18 -std=c11 -O2 -ffunction-sections -fdata-sections -c runtime/gamecode_link.c -o "$dir/link.o"
clang++-18 -std=c++20 -O2 -fuse-ld=lld -ffunction-sections -fdata-sections \
    -I"$deps/capstone-5.0.9/src/include" -I"$src/clang/include" -I"$host/tools/clang/include" \
    -I"$src/llvm/include" -I"$host/include" tools/test_gamecode_identity.cpp "$dir/cache.o" "$dir/link.o" \
    "$host/lib/libLLVMSupport.a" "$crypto/bin/linux/libmbedtls.a" \
    -Wl,--gc-sections $($host/bin/llvm-config --link-static --system-libs support) -pthread -o "$dir/test"
"$dir/test"
