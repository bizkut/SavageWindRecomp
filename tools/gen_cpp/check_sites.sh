#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
main=${1:-build/exefs-1.9.0/main}
out=build/gen_cpp_sites
python=${PYTHON:-tools/venv/bin/python}
mkdir -p "$out"
"$python" tools/gen_cpp/sites_data.py > "$out/sites_data.h"
cmp tools/gen_cpp/sites_data.h "$out/sites_data.h"
if [ "$main" = "--tables-only" ]; then
    exit 0
fi
${CXX:-c++} -std=c++17 -O2 tools/gen_cpp/sites_dump.cpp -o "$out/sites_dump"
"$python" tools/gen_cpp/sites_dump.py "$main" > "$out/python.txt"
"$out/sites_dump" "$main" > "$out/cpp.txt"
diff -u "$out/python.txt" "$out/cpp.txt"
