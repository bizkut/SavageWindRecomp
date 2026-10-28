#!/usr/bin/env bash
# FSR4 RADV probe; generated SDK tables stay in build/. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
sdk_src=build/ps5/fsr4-sdk-src
radv="$root/.deps/ps5/radv"
[[ -f "$sdk_src/build/fsr4-runtime/fsr4_passes.h" ]] || { echo 'missing generated FSR4 tables in build/ps5/fsr4-sdk-src/build/fsr4-runtime' >&2; exit 1; }
python3 -c 'import json, sys
with open(sys.argv[1]) as f: manifest = json.load(f)
needed = {f"{model}-band{band}" for model in ("standard", "ultra-performance") for band in (0, 1)}
missing = needed - {entry["family"] for entry in manifest["families"]}
if missing:
    sys.exit("FSR4 tables missing " + ", ".join(sorted(missing)) +
             "; regenerate with tools/build_fsr4_runtime.py (all families)")' "$sdk_src/build/fsr4-runtime/manifest.json"
python3 ps5/tools/gen-vk-globals.py "$radv/include/vulkan/vulkan_core.h" build/ps5/fsr4-vk-globals.c
export PS5_RADV_ARCHIVE="$radv/libvulkan_radeon.ps5.a"
export PS5_CFLAGS="-I$radv/include -I$root/$sdk_src/include -I$root/$sdk_src/src -I$root/$sdk_src/build/fsr4-runtime ${FSR4_PROBE_CFLAGS:-}"
export PS5_LINK_INPUTS="--gc-sections --wrap=vkCreateComputePipelines"
bash ps5/tools/build-title.sh fsr4-radv ps5/probe/fsr4_radv.c "$sdk_src/src/ps5_fsr4.c" build/ps5/fsr4-vk-globals.c
