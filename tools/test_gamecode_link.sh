#!/bin/sh
set -eu

# Run from the repository root. --real and --cache extract the private PC-built archive.
if [ "${1:-}" = "--cache" ] || [ "${1:-}" = "--real" ]; then
    crypto=build/ps5/importer/host/source/deps/libmbedtls
    clang-18 -std=c11 -Wall -Wextra -Werror -O2 -DGAMECODE_CACHE_TEST -I"$crypto/include" \
        tools/test_gamecode_link.c runtime/gamecode_link.c runtime/gamecode_cache.c "$crypto/bin/linux/libmbedtls.a" \
        -o build/gamecode-link-test
else
    clang-18 -std=c11 -Wall -Wextra -Werror -O2 tools/test_gamecode_link.c runtime/gamecode_link.c \
        -o build/gamecode-link-test
fi
clang-18 -target x86_64-unknown-linux-gnu -fPIC -O1 -ffunction-sections -fdata-sections \
    -c tools/test_gamecode_link_input.c -o build/gamecode-link-input.o
build/gamecode-link-test --synthetic build/gamecode-link-input.o
if [ "${1:-}" = "--real" ] || [ "${1:-}" = "--cache" ]; then
    python3 - <<'PY'
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("exports", "ps5/tools/gen-gamecode-exports.py")
exports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exports)
defined, undefined = exports.symbols("llvm-nm-18", [Path("build/ps5/gamecode/libgamecode.a")])
manifest = {line.strip() for line in Path("runtime/gamecode_export_symbols.txt").read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")}
missing = undefined - defined.keys() - manifest - exports.emitted_names()
if missing:
    raise SystemExit("gamecode export manifest missing: " + ", ".join(sorted(missing)))
print("gamecode export manifest covers archive externals")
PY
    object_dir=$(mktemp -d build/gamecode-link-real.XXXXXX)
    trap 'rm -rf "$object_dir"' EXIT HUP INT TERM
    (cd "$object_dir" && llvm-ar-18 x "$OLDPWD/build/ps5/gamecode/libgamecode.a")
    build/gamecode-link-test "${1:-}" build/ps5/gamecode_exports.c "$object_dir"/*.o
    if [ "${1:-}" = "--real" ]; then
        build/gamecode-link-test --cache build/ps5/gamecode_exports.c "$object_dir"/*.o
    fi
fi
