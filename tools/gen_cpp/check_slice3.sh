#!/usr/bin/env bash
# The native path links only BSD-licensed Capstone; Python/Unicorn is a separate oracle.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
exefs=${1:-"$root/build/exefs-1.9.0"}
out="$root/build/s9-cpp"
python=${PYTHON:-"$root/tools/venv/bin/python"}
mkdir -p "$out"
make -C "$root/tools/gen_cpp" ../../build/s9-cpp/discover
"$python" "$root/tools/layout.py" "$exefs" "$out"
status=0
for module in subsdk0 sdk main; do
    echo "== $module =="
    /usr/bin/time -f 'python wall=%e maxrss_kib=%M' "$python" "$root/tools/gen_cpp/discover_py.py" "$exefs" "$out/layout.txt" "$module" > "$out/$module.discovery.python"
    /usr/bin/time -f 'c++ wall=%e maxrss_kib=%M' "$out/discover" "$exefs" "$out/layout.txt" "$module" > "$out/$module.discovery.cpp"
    if cmp -s "$out/$module.discovery.python" "$out/$module.discovery.cpp"; then
        echo identical
    else
        diff -u "$out/$module.discovery.python" "$out/$module.discovery.cpp" > "$out/$module.discovery.diff" || :
        echo "different: $(wc -l < "$out/$module.discovery.diff") diff lines ($out/$module.discovery.diff)"
        status=1
    fi
done
exit "$status"
