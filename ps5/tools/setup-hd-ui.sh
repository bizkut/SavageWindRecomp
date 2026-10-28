#!/usr/bin/env bash
# Fetch the pinned public model and MIT BC7 encoder (never game artwork).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
work="$root/build/ps5/hd-ui-deps"
mkdir -p "$work"
read -r bc_rev bc_pin < <(awk '$1 == "bc7enc-rdo" { print $(NF-1), $NF }' ps5/deps.lock)
read -r model_pin < <(awk '$1 == "realesrgan-ubuntu" { print $NF }' ps5/deps.lock)
read -r weights_pin < <(awk '$1 == "realesr-animevideov3" { print $NF }' ps5/deps.lock)
fetch() {
    local url=$1 path=$2 pin=$3
    if [[ ! -f "$path" ]] || ! echo "${pin#sha256:}  $path" | sha256sum --check --status; then
        curl -fL "$url" -o "$path"
    fi
    echo "${pin#sha256:}  $path" | sha256sum --check --strict >&2
}
fetch "https://codeload.github.com/richgel999/bc7enc_rdo/tar.gz/$bc_rev" "$work/bc7.tar.gz" "$bc_pin"
fetch "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip" "$work/model.zip" "$model_pin"
mkdir -p "$work/bc7-$bc_rev" "$work/model"
if [[ ! -f "$work/bc7-$bc_rev/bc7enc.cpp" ]]; then
    tar -xf "$work/bc7.tar.gz" -C "$work/bc7-$bc_rev" --strip-components=1
fi
unzip -joq "$work/model.zip" 'models/realesr-animevideov3-x2.param' 'models/realesr-animevideov3-x4.param' 'models/realesr-animevideov3-x4.bin' -d "$work/model"
echo "${weights_pin#sha256:}  $work/model/realesr-animevideov3-x4.bin" | sha256sum --check --strict >&2
printf '%s\n%s\n' "$work/bc7-$bc_rev" "$work/model"
