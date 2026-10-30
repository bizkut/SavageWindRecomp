#!/bin/sh
set -eu

mkdir -p build
${CC:-clang-18} -std=gnu11 -D_GNU_SOURCE -DGAMECODE_CONSOLE -O2 -Wall -Wextra \
    tools/test_gamecode_settings.c runtime/settings.c -o build/gamecode-settings-test
${CC:-clang-18} -std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra \
    tools/test_gamecode_settings.c runtime/settings.c -o build/gamecode-settings-default-test

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
ini="$tmp/savagewind.ini"
build/gamecode-settings-test "$ini" 0 8 1
build/gamecode-settings-test "$ini" 0 8 0
printf 'settings_version = 3\ngamecode_overlap = 1\ngamecode_threads = 12\n' > "$ini"
build/gamecode-settings-test "$ini" 1 12 1
build/gamecode-settings-test "$ini" 1 12 0
printf 'settings_version = 3\ngamecode_overlap = 0\ngamecode_threads = -4\n' > "$ini"
build/gamecode-settings-test "$ini" 0 1 1
build/gamecode-settings-test "$ini" 0 1 0
printf 'settings_version = 3\ngamecode_overlap = 1\ngamecode_threads = 200\n' > "$ini"
build/gamecode-settings-test "$ini" 1 16 1
build/gamecode-settings-test "$ini" 1 16 0
printf 'settings_version = 3\ngamecode_overlap = 1\ngamecode_threads = 12\n' > "$ini"
build/gamecode-settings-default-test "$ini" 0 8 1
