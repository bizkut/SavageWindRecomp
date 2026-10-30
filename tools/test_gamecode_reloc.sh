#!/bin/sh
set -eu
# Private game objects are extracted under build/ and removed on exit.
object_dir=$(mktemp -d build/gamecode-reloc.XXXXXX)
trap 'rm -rf "$object_dir"' EXIT HUP INT TERM
(cd "$object_dir" && llvm-ar-18 x "$OLDPWD/build/ps5/gamecode/libgamecode.a")
clang-18 -std=c11 -Wall -Wextra -Werror -O2 tools/test_gamecode_reloc.c runtime/gamecode_link.c \
    -o build/gamecode-reloc-test
build/gamecode-reloc-test build/ps5/game/llvm-pie.elf build/ps5/game/title.map \
    build/ps5/gamecode_exports.c "$object_dir"/*.o
