#!/usr/bin/env bash
# Savage Wind PS5 - installs the pinned payload SDK (ps5/deps.lock) into .deps/ps5.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The SDK is exported from the pinned revision of ../forever/PS5_PayloadSDK with git archive (never its working
# tree, which other projects share) and installed by that revision's own platform/tools/setup-sdk.sh. The release
# ZIP is taken from ../forever/.downloads when its SHA-256 matches, else setup-sdk.sh downloads it.
# Prints the SDK directory.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
lock="$root/ps5/deps.lock"
pin() { awk -v n="$1" '$1 == n { print $2 " " $3 }' "$lock"; }

read -r sdk_fork_rel sdk_revision < <(pin payload-sdk)
read -r zip_rel zip_pin < <(pin payload-sdk-release)
sdk_fork="$root/$sdk_fork_rel"
cache="$root/.deps/ps5"
sdk="$cache/ps5-payload-sdk"

if [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$sdk_revision" ]]; then
    git -C "$sdk_fork" cat-file -e "$sdk_revision^{commit}" 2>/dev/null || {
        echo "$sdk_fork does not have the pinned revision $sdk_revision" >&2
        exit 2
    }
    mkdir -p "$cache"
    if [[ ! -f $cache/ps5-payload-sdk.zip && -f $root/$zip_rel ]] &&
        [[ "sha256:$(sha256sum "$root/$zip_rel" | cut -d' ' -f1)" == "$zip_pin" ]]; then
        cp "$root/$zip_rel" "$cache/ps5-payload-sdk.zip"
    fi
    tree=$(mktemp -d)
    trap 'rm -rf -- "$tree"' EXIT
    git -C "$sdk_fork" archive "$sdk_revision" | tar -x -C "$tree"
    bash "$tree/platform/tools/setup-sdk.sh" "$sdk" "$sdk_revision" "$cache" >&2
fi
[[ -x $sdk/bin/prospero-lld && -d $sdk/target/include ]] || {
    echo "the pinned payload SDK at $sdk is incomplete" >&2
    exit 2
}
printf '%s\n' "$sdk"
