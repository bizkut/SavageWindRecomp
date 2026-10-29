#!/bin/sh
set -eu

# Run from the repository root. --real additionally extracts the private PC-built archive.
clang-18 -std=c11 -Wall -Wextra -Werror -O2 tools/test_gamecode_link.c runtime/gamecode_link.c \
    -o build/gamecode-link-test
clang-18 -target x86_64-unknown-linux-gnu -fPIC -O1 -ffunction-sections -fdata-sections \
    -c tools/test_gamecode_link_input.c -o build/gamecode-link-input.o
build/gamecode-link-test --synthetic build/gamecode-link-input.o
if [ "${1:-}" = "--real" ]; then
    object_dir=$(mktemp -d build/gamecode-link-real.XXXXXX)
    trap 'rm -rf "$object_dir"' EXIT HUP INT TERM
    (cd "$object_dir" && llvm-ar-18 x "$OLDPWD/build/ps5/gamecode/libgamecode.a")
    build/gamecode-link-test --real build/ps5/gamecode_exports.c "$object_dir"/*.o
fi
