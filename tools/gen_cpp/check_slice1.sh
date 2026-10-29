#!/usr/bin/env bash
# Compare the native parser with the Python implementation using private local ExeFS inputs.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
exefs=${1:-"$root/build/exefs-1.9.0"}
out="$root/build/s9-cpp"
python=${PYTHON:-"$root/tools/venv/bin/python"}
mkdir -p "$out"
make -C "$root/tools/gen_cpp"
"$python" "$root/tools/layout.py" "$exefs" "$out"
status=0
for module in main subsdk0 sdk; do
    echo "== $module =="
    /usr/bin/time -f 'python wall=%e maxrss_kib=%M' "$python" "$root/tools/gen_cpp/dump_py.py" "$exefs" "$out/layout.txt" "$module" > "$out/$module.python.txt"
    /usr/bin/time -f 'c++ wall=%e maxrss_kib=%M' "$out/dump" "$exefs" "$out/layout.txt" "$module" > "$out/$module.cpp.txt"
    if cmp -s "$out/$module.python.txt" "$out/$module.cpp.txt"; then
        echo 'identical'
    else
        diff -u "$out/$module.python.txt" "$out/$module.cpp.txt" > "$out/$module.diff" || :
        echo "different: $(wc -l < "$out/$module.diff") diff lines ($out/$module.diff)"
        status=1
    fi
done
exit "$status"
