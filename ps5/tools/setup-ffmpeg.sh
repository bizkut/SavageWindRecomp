#!/usr/bin/env bash
# Savage Wind PS5 - the FFmpeg the title's movies link (runtime/hle/movie.c, plan D9): pacbrew's ps5-payload-ffmpeg
# package as installed in the payload SDK's homebrew prefix, used read-only and never built here.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Refuses a mismatch with ps5/deps.lock: the pacbrew-repo checkout must be at the pinned revision; each package whose
# archives are linked (FFmpeg and the libraries its archives name: OpenSSL, bzip2, xz, libiconv) must be installed at
# that revision's recipe version; and the archives must have the pinned digest (pacbrew-ffmpeg: SHA-256 of their
# sha256sum listing, in the order below). zlib is RADV's (build-game.sh), not pacbrew's.
# Prints the prefix, then the archives in link order, one per line.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
lock="$root/ps5/deps.lock"
read -r repo_rel revision < <(awk '$1 == "pacbrew-repo" { print $2 " " $NF }' "$lock")
read -r prefix digest < <(awk '$1 == "pacbrew-ffmpeg" { print $2 " " $NF }' "$lock")
repo="$root/$repo_rel"
pacman_db=${PS5_PACMAN_DB:-/var/lib/pacman/local}

head=$(git -C "$repo" rev-parse HEAD 2>/dev/null) || { echo "no pacbrew-repo checkout at $repo" >&2; exit 2; }
[[ $head == "$revision" ]] || { echo "$repo is at $head, ps5/deps.lock pins $revision" >&2; exit 2; }

# recipe directory:archives it provides
packages=(ffmpeg:"libavformat.a libavcodec.a libswscale.a libswresample.a libavutil.a"
    openssl:"libssl.a libcrypto.a" bzip2:libbz2.a xz:liblzma.a libiconv:libiconv.a)
archives=()
for entry in "${packages[@]}"; do
    recipe=${entry%%:*}
    pkgbuild=$(git -C "$repo" show "$revision:$recipe/PKGBUILD")
    name=$(sed -n 's/^pkgname=//p' <<<"$pkgbuild")
    version=$(sed -n 's/^pkgver=//p' <<<"$pkgbuild")-$(sed -n 's/^pkgrel=//p' <<<"$pkgbuild")
    [[ -f $pacman_db/$name-$version/desc ]] || {
        echo "pacbrew package $name $version (recipe $recipe/PKGBUILD at $revision) is not installed" >&2
        exit 2
    }
    for archive in ${entry#*:}; do archives+=("$prefix/lib/$archive"); done
done
actual=$(cd "$prefix/lib" && sha256sum "${archives[@]##*/}" | sha256sum | cut -d' ' -f1)
[[ "sha256:$actual" == "$digest" ]] || {
    echo "FFmpeg archives in $prefix/lib (sha256:$actual) do not match ps5/deps.lock ($digest)" >&2
    exit 2
}
[[ -f $prefix/include/libavformat/avformat.h ]] || { echo "missing FFmpeg headers in $prefix/include" >&2; exit 2; }
printf '%s\n' "$prefix" "${archives[@]}"
