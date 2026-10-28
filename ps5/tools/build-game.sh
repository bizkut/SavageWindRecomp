#!/usr/bin/env bash
# Build the user's generated 1.9.0 game code into the PS5 title: runtime, NVN renderer on RADV (VideoOut display
# plane, runtime/ps5/window.c) and shaderc, package library.
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
# zlib's headers; its code is the RADV archive's (Mesa's zlib 1.3.1 subproject, linked whole: same API and ABI)
zlib="$root/../forever/PS5_Vulkan/.deps/native/zlib/zlib-1.3.2"
[[ -f $zlib/zlib.h ]] || { echo "missing zlib headers: $zlib" >&2; exit 1; }

# RADV (pinned in ps5/deps.lock) and shaderc (ps5/tools/build-shaderc.sh, plan D6)
radv="$root/.deps/ps5/radv"
radv_pin=$(awk '$1 == "radv-archive" { print $3 }' ps5/deps.lock)
[[ "sha256:$(sha256sum "$radv/libvulkan_radeon.ps5.a" | cut -d' ' -f1)" == "$radv_pin" ]] ||
    { echo "RADV archive $radv/libvulkan_radeon.ps5.a does not match ps5/deps.lock" >&2; exit 1; }
shaderc=$(bash ps5/tools/build-shaderc.sh)

# Generated inputs: the Vulkan commands (no loader in a title), SMAA's shader as a C string (as the Makefile makes it)
gen="$root/build/ps5/game-gen"
mkdir -p "$gen/inc"
python3 ps5/tools/gen-vk-globals.py "$radv/include/vulkan/vulkan_core.h" "$gen/vk_globals.c"
LC_ALL=C tr -d '\r' < third_party/smaa/SMAA.hlsl | LC_ALL=C tr '\200-\377' '?' |
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' > "$gen/inc/smaa_hlsl.inc"
# The shader caches' ids, as the Makefile computes them (TRANSLATOR_ID, SPIRV_ID): caches survive a rebuild
translator_id=$(cat runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h | sha256sum | cut -c1-16)
spirv_id=$(sha256sum < runtime/nvn/vulkan/spirv.c | cut -c1-16)

export PS5_RADV_ARCHIVE="$radv/libvulkan_radeon.ps5.a"
export PS5_CFLAGS="-fexceptions -Iruntime -Iruntime/nvn -Iruntime/ps5/include -Ithird_party -I$radv/include \
-I$shaderc/include -isystem third_party/stb -I$gen/inc -Ithird_party/smaa/Textures -I$zlib -DROMFS_PACKAGE \
-DTRANSLATOR_ID=\"$translator_id\" -DSPIRV_ID=\"$spirv_id\" -march=x86-64-v3 -Wno-format"
export PS5_LINK_INPUTS="--gc-sections --wrap=exit --wrap=_exit --wrap=_Exit --wrap=quick_exit --wrap=abort --wrap=sceSystemServiceLoadExec --start-group $game $package/libsavagewind-package.a $ns/libpietendo/bin/linux/libpietendo.a $ns/libtoolchain/bin/linux/libtoolchain.a $ns/libfmt/bin/linux/libfmt.a $ns/liblz4/bin/linux/liblz4.a $ns/libmbedtls/bin/linux/libmbedtls.a $shaderc/lib/libshaderc_combined.a --end-group"
bash ps5/tools/build-title.sh game \
    runtime/platform/main_ps5.c runtime/platform/ps5.c runtime/platform/gamelib_ps5.c \
    runtime/platform/exefs_ps5.c runtime/ps5/sdl_shim.c runtime/ps5/log.c runtime/ps5/unavailable.c runtime/ps5/diagnostics.cpp \
    runtime/ps5/window.c build/ps5/game-gen/vk_globals.c \
    runtime/dispatch.c runtime/fnprof.c runtime/ipc.c runtime/kernel.c runtime/loader.c runtime/imageload.c \
    runtime/platform/paths_ps5.c runtime/settings.c runtime/language.c runtime/storage_ps5.c \
    runtime/romfs_build.c runtime/romfs_image.c ps5/storagebroker/client.c \
    runtime/hle/heaps.c runtime/hle/setjmp.c runtime/hle/pl.c runtime/hle/yaz0.c runtime/hle/vibration.c runtime/hle/vi.c \
    runtime/hle/nvn_boot.c runtime/hle/framepace.c runtime/hle/npad.c runtime/hle/motion.c \
    runtime/nvn/astc.c runtime/nvn/cmdbuf.c runtime/nvn/formats.c runtime/nvn/objects.c runtime/nvn/output_scale.c \
    runtime/nvn/shader_archive.c runtime/nvn/smaa_data.c runtime/nvn/tegra.c runtime/nvn/maxwell/translate.c \
    runtime/nvn/vulkan/device.c runtime/nvn/vulkan/exec.c runtime/nvn/vulkan/textures.c runtime/nvn/vulkan/shaders.c \
    runtime/nvn/vulkan/spirv.c runtime/nvn/vulkan/precompile.c runtime/nvn/vulkan/vma.cpp \
    runtime/services/acc.c runtime/services/am.c runtime/services/audio.c runtime/services/fs.c runtime/services/hid_set.c \
    runtime/services/misc.c runtime/services/time.c runtime/services/test_warp.c
