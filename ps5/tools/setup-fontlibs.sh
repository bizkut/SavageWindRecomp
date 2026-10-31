#!/usr/bin/env bash
# Build the title's FreeType from pinned upstream source with the pinned payload SDK.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
export PS5_PAYLOAD_SDK="$sdk"
url=$(awk '$1 == "freetype-source" { print $2 }' "$root/ps5/deps.lock")
pin=sha256:$(awk '$1 == "freetype-sha256" { print $2 }' "$root/ps5/deps.lock")
[[ -n $url && $pin =~ ^sha256:[0-9a-f]{64}$ ]] || { echo 'missing FreeType source pin' >&2; exit 2; }
cache="$root/.deps/ps5"
archive="$cache/downloads/freetype-2.13.2.tar.xz"
prefix="$cache/fontlibs"
build="$cache/fontlibs-build"
mkdir -p "$cache/downloads"
exec 9>"$cache/fontlibs.lock"
flock 9
if [[ ! -f $archive ]]; then
    curl -fL --retry 3 -o "$archive.tmp" "$url" >&2
    mv "$archive.tmp" "$archive"
fi
[[ "sha256:$(sha256sum "$archive" | cut -d' ' -f1)" == "$pin" ]] || {
    echo "FreeType source digest mismatch: $archive" >&2; exit 2;
}
key=$( { sha256sum "$0"; printf '%s\n' "$pin"; cat "$sdk/.ps5-sdk-revision"; } | sha256sum | cut -d' ' -f1)
if [[ ! -f $prefix/.build-key || $(<"$prefix/.build-key") != "$key" ]]; then
    rm -rf -- "$build" "$prefix"
    mkdir -p "$build/src" "$build/obj" "$prefix"
    tar -xJf "$archive" --strip-components=1 -C "$build/src"
    (
        cd "$build/obj"
        CC="$sdk/bin/prospero-clang" AR="$sdk/bin/prospero-ar" RANLIB="$sdk/bin/prospero-ranlib" \
            "$build/src/configure" --host=x86_64-unknown-freebsd --prefix="$prefix" \
            --enable-static --disable-shared --without-zlib --without-bzip2 --without-png \
            --without-harfbuzz --without-brotli >&2
        make -j"${PS5_FONTLIBS_JOBS:-4}" >&2
        make install >&2
    )
    printf '%s\n' "$key" > "$prefix/.build-key"
fi
printf '%s\n' "$prefix"
