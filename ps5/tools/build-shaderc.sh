#!/usr/bin/env bash
# Savage Wind PS5 - shaderc (with glslang, SPIRV-Tools) as static archives for the PS5 target (plan D6, spike S5).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Same shaderc release and dependency pins as the desktop build (tools/linux/build_deps.sh: v2026.3, its DEPS file
# pins glslang / SPIRV-Tools / SPIRV-Headers). Built with the pinned payload SDK's prospero.cmake toolchain into
# .deps/ps5/shaderc/{include,lib}. Prints the prefix.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
prefix="$root/.deps/ps5/shaderc"
work="$root/build/ps5/shaderc"
url=https://github.com/google/shaderc/archive/refs/tags/v2026.3.tar.gz
sha=ee493ccf1b3038b4ef2fe024664c5eb2dc4bcc1f6b05b33e3909de0e19c81024

if [[ -f $prefix/lib/libshaderc_combined.a ]]; then printf '%s\n' "$prefix"; exit 0; fi
mkdir -p "$work"
tarball="$work/shaderc-2026.3.tar.gz"
if [[ ! -f $tarball ]] || ! echo "$sha  $tarball" | sha256sum --check --status; then
    src="$root/build/linux/x86_64/deps-src/shaderc-2026.3.tar.gz"
    if [[ -f $src ]] && echo "$sha  $src" | sha256sum --check --status; then cp "$src" "$tarball"
    else wget -q "$url" -O "$tarball"; echo "$sha  $tarball" | sha256sum --check --strict >&2; fi
fi
rm -rf "$work/src" && mkdir -p "$work/src" && tar -xf "$tarball" -C "$work/src" --strip-components 1
(cd "$work/src" && python3 utils/git-sync-deps >&2)

PS5_PAYLOAD_SDK="$sdk" cmake -S "$work/src" -B "$work/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-femulated-tls -fno-stack-protector" -DCMAKE_CXX_FLAGS="-femulated-tls -fno-stack-protector" \
    -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF \
    -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF -DSHADERC_ENABLE_SHARED_CRT=OFF >&2
PS5_PAYLOAD_SDK="$sdk" cmake --build "$work/build" --target shaderc_combined -j"$(nproc)" >&2
mkdir -p "$prefix/include/shaderc" "$prefix/lib"
cp "$work/src/libshaderc/include/shaderc/"*.h "$prefix/include/shaderc/"
cp "$work/build/libshaderc/libshaderc_combined.a" "$prefix/lib/"
printf '%s\n' "$prefix"
