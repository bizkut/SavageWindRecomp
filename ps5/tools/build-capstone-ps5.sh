#!/usr/bin/env bash
# Cross-build the generator's pinned ARM64-only Capstone static library.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
deps="$root/build/s9-cpp/deps"
archive="$deps/capstone-5.0.9.tar.gz"
source="$deps/capstone-5.0.9/src"
out="$root/build/ps5/capstone"
mkdir -p "$deps" "$out"
if [[ ! -f $archive ]]; then
    "${PYTHON:-$root/tools/venv/bin/python}" -m pip download --no-deps --no-binary :all: capstone==5.0.9 -d "$deps"
fi
sha=0429af292ddc604d3c9344696807e281d5e728db029f00e6a4ea9e3bff1aac9e
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check --strict
if [[ ! -f $source/CMakeLists.txt ]]; then tar -xf "$archive" -C "$deps"; fi
flags="-target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default -isysroot $sdk -isystem $sdk/target/include"
flags+=" -fno-stack-protector -fno-plt -femulated-tls"
cmake -S "$source" -B "$out" -G Ninja \
    -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_C_COMPILER=clang-18 \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_AR="$sdk/bin/llvm-ar" -DCMAKE_RANLIB="$sdk/bin/llvm-ranlib" \
    -DCMAKE_BUILD_TYPE=Release -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON \
    -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build "$out" --parallel "${JOBS:-2}"
