#!/usr/bin/env bash
# Build the same in-memory generator probe for host and PS5; no game-derived output is committed.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
deps="$root/build/s9-cpp/deps"
input="$root/build/ps5/s9-input"
mkdir -p "$input" "$root/build/ps5/variants/s9"
bash "$root/tools/gen_cpp/setup_capstone.sh"
bash "$root/ps5/tools/build-capstone-ps5.sh"
cp "$root/build/exefs-1.9.0/"{main,subsdk0,sdk} "$input/"
cp "$root/build/s9-cpp/layout.txt" "$input/layout.txt"
cp "$root/runtime/hle/hle_symbols.txt" "$input/hle_symbols.txt"
${CXX:-c++} -std=c++20 -O2 -I"$deps/capstone-5.0.9/src/include" \
    "$root/ps5/probe/s9_gen.cpp" "$root/tools/gen_cpp/patches.cpp" "$deps/capstone-build/libcapstone.a" \
    -o "$root/build/ps5/s9-host"
export PS5_CFLAGS="-fexceptions -I$deps/capstone-5.0.9/src/include"
export PS5_LINK_INPUTS="$root/build/ps5/capstone/libcapstone.a"
bash "$root/ps5/tools/build-title.sh" s9 ps5/probe/s9_gen.cpp tools/gen_cpp/patches.cpp
rm -rf -- "$root/build/ps5/variants/s9/PPSA90212"
cp -a "$root/ps5/dist/s9/PPSA90212" "$root/build/ps5/variants/s9/"
"$root/build/ps5/s9-host" "$input" > "$root/build/ps5/s9-host-hashes-all.txt"
printf '%s\n' "Upload $input/{main,subsdk0,sdk,layout.txt,hle_symbols.txt} to /data/savagewind/s9/" \
    "Host hashes: $root/build/ps5/s9-host-hashes-all.txt"
sha256sum "$root/build/ps5/variants/s9/PPSA90212/eboot.bin"
