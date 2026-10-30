#!/bin/sh
set -eu
# Run from the repository root; expose only the SDK platform headers to the host compiler.
inc=$(mktemp -d build/ps5-guest-test.XXXXXX)
trap 'rm -rf "$inc"' EXIT HUP INT TERM
ln -s "$PWD/.deps/ps5/ps5-payload-sdk/target/include/ps5platform" "$inc/ps5platform"
clang-18 -std=gnu11 -O2 -ffunction-sections -fdata-sections -I"$inc" tools/test_ps5_guest_commit.c \
    -Wl,--gc-sections -pthread -o "$inc/test"
"$inc/test"
