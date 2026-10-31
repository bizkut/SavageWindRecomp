#!/usr/bin/env bash
# Build and package the PS5 console-only title from a clean, tagged source tree.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
candidate=0
if [[ ${1:-} == --candidate && $# == 1 ]]; then
    candidate=1
elif (( $# )); then
    echo "usage: $0 [--candidate]" >&2; exit 2
fi
commit=$(git rev-parse HEAD)
if (( candidate )); then
    tag="candidate-$(git rev-parse --short=12 HEAD)-dirty"
else
    [[ -z $(git status --porcelain --untracked-files=normal) ]] || {
        echo 'Release requires a clean worktree, including untracked files.' >&2; exit 1;
    }
    mapfile -t tags < <(git tag --points-at HEAD --list 'ps5-v*')
    [[ ${#tags[@]} == 1 && ${tags[0]} =~ ^ps5-v[0-9A-Za-z][0-9A-Za-z._-]*$ ]] || {
        echo 'Release requires exactly one ps5-v* tag at HEAD.' >&2; exit 1;
    }
    tag=${tags[0]}
fi
[[ -f ps5/release/SOURCE.txt && -d ps5/release/notices ]] || {
    echo 'Corresponding source and licence notices are required before packaging.' >&2; exit 1;
}
if (( ! candidate )); then
    [[ $(<ps5/release/SOURCE.txt) != *UNRESOLVED* ]] || {
        echo 'SOURCE.txt has unresolved release blockers; public packaging is prohibited.' >&2; exit 1;
    }
    make -C ps5/storagebroker
    PS5_GAMECODE=console bash ps5/tools/build-game.sh
fi
mkdir -p build/ps5/releases
python3 ps5/tools/package-public-release.py "$tag" "$commit" "$candidate"
