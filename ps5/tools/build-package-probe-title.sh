#!/usr/bin/env bash
# Build the package benchmark as an unelevated PS5 title.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
manifest=${1:-/tmp/savagewind-import-nsp.sha256}
folder=${2:-"$root/build/romfs-reference/game/romfs"}
sdk=$(bash ps5/tools/setup-deps.sh)
work="$root/build/ps5/importer"
[[ -f $work/ps5-build/build.ninja ]] || bash ps5/tools/build-importer.sh
python3 ps5/importer/probe_samples.py "$manifest" "$folder" "$work/ps5-build/package_probe_samples.h"
PS5_PAYLOAD_SDK="$sdk" cmake --build "$work/ps5-build" -j1 --target savagewind-package
ns="$work/nstool-source/deps"
export PS5_CFLAGS="-fexceptions -DROMFS_PACKAGE -I$work/ps5-build -I$ns/libtoolchain/include -I$ns/libpietendo/include -I$ns/libmbedtls/include -I$ns/libfmt/include -I$ns/liblz4/include"
export PS5_LINK_INPUTS="--gc-sections --start-group $work/ps5-build/libsavagewind-package.a $ns/libpietendo/bin/linux/libpietendo.a $ns/libtoolchain/bin/linux/libtoolchain.a $ns/libmbedtls/bin/linux/libmbedtls.a $ns/libfmt/bin/linux/libfmt.a $ns/liblz4/bin/linux/liblz4.a --end-group"
name=package-probe-title
sources=(ps5/probe/package_probe_title.cpp runtime/romfs_image.c runtime/romfs_build.c)
if [[ ${PS5_PROBE_GAMEHEAP:-0} == 1 ]]; then
    name=package-probe-gameheap
    sources=(ps5/probe/package_probe_gameheap.cpp runtime/platform/ps5.c runtime/ps5/log.c runtime/ps5/diagnostics.cpp
             runtime/romfs_image.c runtime/romfs_build.c)
    PS5_CFLAGS+=" -march=x86-64-v3"
    PS5_LINK_INPUTS+=" --wrap=exit --wrap=_exit --wrap=_Exit --wrap=quick_exit --wrap=abort --wrap=sceSystemServiceLoadExec"
fi
bash ps5/tools/build-title.sh "$name" "${sources[@]}"
sha256sum "ps5/dist/$name/PPSA90212/eboot.bin"
