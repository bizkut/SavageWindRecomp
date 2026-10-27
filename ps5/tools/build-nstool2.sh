#!/usr/bin/env bash
# Cross-build the pinned nstool2 + pinned submodules as a PS5 payload dependency.
# Source stays read-only in ../nstool2; the complete snapshot and build live under build/ps5/nstool2.
# SPDX-License-Identifier: GPL-3.0-or-later (script); nstool2 and dependencies retain their own licences.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
src_repo="$root/../nstool2"
pin=7e0a49b61ae98b730900709e3ac5ba1945c1906e
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
work="$root/build/ps5/nstool2"
source="$work/source"
build="$work/build"

[[ $(git -C "$src_repo" rev-parse HEAD) == "$pin" ]] || { echo "nstool2 pin mismatch" >&2; exit 2; }
[[ -z $(git -C "$src_repo" status --porcelain --untracked-files=no) ]] || { echo "nstool2 tracked tree is dirty" >&2; exit 2; }
if git -C "$src_repo" submodule status --recursive | grep -qE '^[+-]'; then
    echo "nstool2 submodules do not match the pinned gitlinks" >&2
    exit 2
fi
rm -rf "$source" "$build"
mkdir -p "$source"
# Copy the checked, pinned source and populated submodule worktrees without git metadata or their build products.
tar -C "$src_repo" --exclude=.git --exclude='./bin' --exclude='./build' -cf - . | tar -C "$source" -xf -


PS5_PAYLOAD_SDK="$sdk" cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release -DPLATFORM_NAME=ps5 \
    -DCMAKE_C_FLAGS="-femulated-tls -fno-stack-protector" \
    -DCMAKE_CXX_FLAGS="-femulated-tls -fno-stack-protector" >&2
PS5_PAYLOAD_SDK="$sdk" cmake --build "$build" -j"$(nproc)" >&2
binary="$source/bin/ps5/nstool"
[[ -x $binary ]] || { echo "nstool2 PS5 binary was not produced" >&2; exit 2; }
printf '%s\n' "$binary"
