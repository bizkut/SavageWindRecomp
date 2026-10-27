#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the importer against a private snapshot of the pinned nstool source.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
repo="$root/../nstool2"
pin=7e0a49b61ae98b730900709e3ac5ba1945c1906e
work="$root/build/ps5/importer"
src="$work/nstool-source"
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
[[ $(git -C "$repo" rev-parse HEAD) == "$pin" ]] || { echo 'nstool2 pin mismatch' >&2; exit 2; }
[[ -z $(git -C "$repo" status --porcelain --untracked-files=no) ]] || { echo 'nstool2 tracked tree is dirty' >&2; exit 2; }
if git -C "$repo" submodule status --recursive | grep -qE '^[+-]'; then
    echo 'nstool2 submodules do not match the pin' >&2; exit 2
fi
rm -rf "$src" "$work/ps5-build"
mkdir -p "$src"
tar -C "$repo" --exclude=.git --exclude='./bin' --exclude='./build' --exclude='*.o' -cf - . | tar -C "$src" -xf -
patch -s -d "$src" -p1 < "$root/ps5/importer/nstool-streams.patch"
PS5_PAYLOAD_SDK="$sdk" cmake -S "$root/ps5/importer" -B "$work/ps5-build" -G Ninja \
    -DNSTOOL_SOURCE="$src" -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DPLATFORM_NAME=ps5 \
    -DCMAKE_C_FLAGS='-femulated-tls -fno-stack-protector' \
    -DCMAKE_CXX_FLAGS='-femulated-tls -fno-stack-protector'
PS5_PAYLOAD_SDK="$sdk" cmake --build "$work/ps5-build" -j2 --target savagewind-import
cp "$work/ps5-build/savagewind-import" "$work/savagewind-import.elf"
printf '%s\n' "$work/savagewind-import.elf"
