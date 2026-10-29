#!/usr/bin/env bash
# Host-only Capstone 5.0.9; the generator does not link Unicorn.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
dir="$root/build/s9-cpp/deps"
archive="$dir/capstone-5.0.9.tar.gz"
mkdir -p "$dir"
if [[ ! -f $archive ]]; then
    "${PYTHON:-$root/tools/venv/bin/python}" -m pip download --no-deps --no-binary :all: capstone==5.0.9 -d "$dir"
fi
echo "0429af292ddc604d3c9344696807e281d5e728db029f00e6a4ea9e3bff1aac9e  $archive" | sha256sum -c -
if [[ ! -f $dir/capstone-5.0.9/src/CMakeLists.txt ]]; then tar -xf "$archive" -C "$dir"; fi
cmake -S "$dir/capstone-5.0.9/src" -B "$dir/capstone-build" \
    -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON \
    -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF \
    -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$dir/capstone-build" -j "${JOBS:-2}"
