#!/usr/bin/env bash
# Build the Real-ESRGAN release's ncnn against the title's shaderc glslang.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
sdk=$(bash ps5/tools/setup-deps.sh)
bash ps5/tools/build-shaderc.sh >/dev/null
work="$root/build/ps5/ncnn"
glslang="$root/build/ps5/shaderc/src/third_party"
read -r rev pin < <(awk '$1 == "ncnn" { print $(NF-1), $NF }' ps5/deps.lock)
sha=${pin#sha256:}
[[ $rev && $pin == sha256:* && ${#sha} == 64 ]] || { echo "invalid ncnn pin" >&2; exit 1; }
[[ -f "$glslang/glslang/glslang/Public/ShaderLang.h" ]] || {
    echo "ncnn requires shaderc's glslang source at $glslang/glslang" >&2; exit 1;
}
mkdir -p "$work"
archive="$work/ncnn-$rev.tar.gz"
if [[ ! -f "$archive" ]] || ! echo "$sha  $archive" | sha256sum --check --status; then
    curl -fL "https://codeload.github.com/Tencent/ncnn/tar.gz/$rev" -o "$archive"
fi
echo "$sha  $archive" | sha256sum --check --strict >&2
patch_id=$(sha256sum ps5/patches/ncnn-shaderc-glslang.patch | cut -c1-16)
tree="$work/$rev-$patch_id"
if [[ ! -f "$tree/src/.savagewind-patched" ]]; then
    mkdir -p "$tree/src"
    tar -xf "$archive" -C "$tree/src" --strip-components=1
    patch -d "$tree/src" -p1 < ps5/patches/ncnn-shaderc-glslang.patch >&2
    touch "$tree/src/.savagewind-patched"
fi
PS5_PAYLOAD_SDK="$sdk" cmake -S "$tree/src" -B "$tree/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS='-femulated-tls -fno-stack-protector' \
    -DCMAKE_CXX_FLAGS='-femulated-tls -fno-stack-protector' \
    -DNCNN_VULKAN=ON -DNCNN_OPENMP=OFF -DNCNN_INSTALL_SDK=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF \
    -DVulkan_INCLUDE_DIR="$root/.deps/ps5/radv/include" \
    -DVulkan_LIBRARY="$root/.deps/ps5/radv/libvulkan_radeon.ps5.a" \
    -DSW_GLSLANG_PARENT="$glslang" >&2
PS5_PAYLOAD_SDK="$sdk" cmake --build "$tree/build" -j1 --target ncnn >&2
printf '%s\n' "$tree"
