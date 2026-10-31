#!/usr/bin/env bash
# Normalize paths in the cross-built RADV archive while preserving debug information.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
args=("$@")
if [[ ${1:-} == setup ]]; then
    for arg in "${args[@]}"; do
        if [[ $arg == --cross-file ]]; then
            work=$(dirname -- "$2")
            sdk=${PS5_RADV_SDK_MAP:?PS5_RADV_SDK_MAP must name the SDK}
            flags="-ffile-prefix-map=$work=/radv -ffile-prefix-map=$sdk=/ps5-sdk -ffile-prefix-map=$(realpath "$sdk")=/ps5-sdk"
            args+=("-Dc_args=$flags" "-Dcpp_args=$flags")
            break
        fi
    done
fi
exec meson "${args[@]}"
