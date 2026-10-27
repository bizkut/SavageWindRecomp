#!/usr/bin/env bash
# Build the user's generated 1.9.0 game code into the M2 PS5 title.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
sdk=$(bash ps5/tools/setup-deps.sh)
game="$root/build/ps5/gamecode/libgamecode.a"
[[ -f $game ]] || { echo 'Build the user game code first with ps5/tools/build-gamecode.sh' >&2; exit 1; }
package="$root/build/ps5/importer/ps5-build"
[[ -f $package/build.ninja ]] || bash ps5/tools/build-importer.sh
PS5_PAYLOAD_SDK="$sdk" cmake --build "$package" -j1 --target savagewind-package
ns="$root/build/ps5/importer/nstool-source/deps"
zlib="$root/../forever/PS5_Vulkan/.deps/native/zlib/zlib-1.3.2"
[[ -f $zlib/zlib.h ]] || { echo "missing zlib source: $zlib" >&2; exit 1; }
export PS5_CFLAGS="-fexceptions -Iruntime -Iruntime/ps5/include -Ithird_party -I$zlib -DROMFS_PACKAGE -march=x86-64-v3 -Wno-format"
export PS5_LINK_INPUTS="--gc-sections --wrap=exit --wrap=_exit --wrap=_Exit --wrap=quick_exit --wrap=abort --wrap=sceSystemServiceLoadExec --start-group $game $package/libsavagewind-package.a $ns/libpietendo/bin/linux/libpietendo.a $ns/libtoolchain/bin/linux/libtoolchain.a $ns/libfmt/bin/linux/libfmt.a $ns/liblz4/bin/linux/liblz4.a $ns/libmbedtls/bin/linux/libmbedtls.a --end-group"
zsrc=()
for file in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite inflate infback inftrees inffast trees uncompr zutil; do
    zsrc+=("../forever/PS5_Vulkan/.deps/native/zlib/zlib-1.3.2/$file.c")
done
bash ps5/tools/build-title.sh game \
    runtime/platform/main_ps5.c runtime/platform/ps5.c runtime/platform/gamelib_ps5.c \
    runtime/platform/exefs_ps5.c runtime/ps5/sdl_shim.c runtime/ps5/log.c runtime/ps5/unavailable.c runtime/ps5/diagnostics.cpp \
    runtime/dispatch.c runtime/fnprof.c runtime/ipc.c runtime/kernel.c runtime/loader.c \
    runtime/platform/paths_ps5.c runtime/settings.c runtime/language.c runtime/storage_ps5.c \
    runtime/romfs_build.c runtime/romfs_image.c ps5/storagebroker/client.c \
    runtime/hle/heaps.c runtime/hle/setjmp.c runtime/hle/pl.c runtime/hle/yaz0.c runtime/hle/vibration.c runtime/hle/vi.c \
    runtime/services/acc.c runtime/services/am.c runtime/services/audio.c runtime/services/fs.c runtime/services/hid_set.c \
    runtime/services/misc.c runtime/services/time.c runtime/services/test_warp.c "${zsrc[@]}"
