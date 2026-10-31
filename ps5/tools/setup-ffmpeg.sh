#!/usr/bin/env bash
# Build the title's FFmpeg from pinned upstream source with the pinned payload SDK.
# SPDX-License-Identifier: GPL-3.0-or-later
# Prints the installation prefix and its static archives, one per line.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
lock="$root/ps5/deps.lock"
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
export PS5_PAYLOAD_SDK="$sdk"
read -r url pin < <(awk '$1 == "ffmpeg-source" { print $2, $3 }' "$lock")
[[ -n $url && $pin == sha256:* ]] || { echo 'missing FFmpeg source pin' >&2; exit 2; }
cache="$root/.deps/ps5"
archive="$cache/downloads/ffmpeg-7.0.1.tar.xz"
prefix="$cache/ffmpeg"
build="$cache/ffmpeg-build"
mkdir -p "$cache"
exec 9>"$cache/ffmpeg.lock"
flock 9
mkdir -p "$(dirname "$archive")"
if [[ ! -f $archive ]]; then
    curl -fL --retry 3 -o "$archive.tmp" "$url" >&2
    mv "$archive.tmp" "$archive"
fi
[[ "sha256:$(sha256sum "$archive" | cut -d' ' -f1)" == "$pin" ]] || {
    echo "FFmpeg source digest mismatch: $archive" >&2; exit 2;
}
# Changing the recipe or SDK revision invalidates the build, including after an interrupted install.
key=$( { sha256sum "$0"; printf '%s\n' "$pin"; cat "$sdk/.ps5-sdk-revision"; } | sha256sum | cut -d' ' -f1)
if [[ ! -f $prefix/.build-key || $(<"$prefix/.build-key") != "$key" ]]; then
    rm -rf -- "$build" "$prefix"
    mkdir -p "$build/src" "$build/obj" "$prefix"
    tar -xJf "$archive" --strip-components=1 -C "$build/src"
    (
        cd "$build/obj"
        "$build/src/configure" --prefix="$prefix" --enable-cross-compile --arch=x86_64 --target-os=freebsd \
            --cc="$sdk/bin/prospero-clang" --cxx="$sdk/bin/prospero-clang++" --nm="$sdk/bin/prospero-nm" \
            --strip="$sdk/bin/prospero-strip" --ar="$sdk/bin/prospero-ar" --ranlib="$sdk/bin/prospero-ranlib" \
            --enable-static --disable-shared --enable-version3 --disable-autodetect --disable-network \
            --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter \
            --disable-postproc >&2
        make -j"${PS5_FFMPEG_JOBS:-4}" >&2
        make install >&2
    )
    printf '%s\n' "$key" > "$prefix/.build-key"
fi
printf '%s\n' "$prefix" "$prefix/lib/libavformat.a" "$prefix/lib/libavcodec.a" \
    "$prefix/lib/libswscale.a" "$prefix/lib/libswresample.a" "$prefix/lib/libavutil.a"
