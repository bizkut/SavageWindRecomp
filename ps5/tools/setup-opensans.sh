#!/usr/bin/env bash
# Fetch the exact OFL-licensed Open Sans variable font used for game text.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
revision=$(awk '$1 == "opensans-revision" { print $NF }' "$root/ps5/deps.lock")
expected=$(awk '$1 == "opensans-font" { print $NF }' "$root/ps5/deps.lock")
font="$root/.deps/ps5/opensans/OpenSans.ttf"
mkdir -p "$(dirname "$font")"
if [[ ! -f $font ]]; then
    tmp="$font.tmp-$$"
    trap 'rm -f "$tmp"' EXIT
    curl -fL "https://raw.githubusercontent.com/googlefonts/opensans/$revision/fonts/variable/OpenSans%5Bwdth%2Cwght%5D.ttf" -o "$tmp" >&2
    [[ "sha256:$(sha256sum "$tmp" | cut -d' ' -f1)" == "$expected" ]] || {
        echo 'Open Sans download does not match ps5/deps.lock' >&2; exit 1;
    }
    mv "$tmp" "$font"
fi
[[ "sha256:$(sha256sum "$font" | cut -d' ' -f1)" == "$expected" ]] || {
    echo 'Open Sans font does not match ps5/deps.lock' >&2; exit 1;
}
printf '%s\n' "$font"
