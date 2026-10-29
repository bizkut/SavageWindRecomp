#!/usr/bin/env bash
# Differential all distinct text words at a fixed PC. Nonzero while any class is unported or differs.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
exefs=${1:-"$root/build/exefs-1.9.0"}
out="$root/build/s9-cpp"
python=${PYTHON:-"$root/tools/venv/bin/python"}
mkdir -p "$out"
make -C "$root/tools/gen_cpp"
status=0
for module in subsdk0 sdk main synthetic; do
    input="$exefs/$module"
    [[ $module == synthetic ]] && input=synthetic
    echo "== $module =="
    /usr/bin/time -f 'python wall=%e maxrss_kib=%M' "$python" "$root/tools/gen_cpp/lift_words.py" "$input" "$out/$module.words" > "$out/$module.lift.python"
    /usr/bin/time -f 'c++ wall=%e maxrss_kib=%M' "$out/lift_words" < "$out/$module.words" > "$out/$module.lift.cpp"
    "$python" "$root/tools/gen_cpp/lift_compare.py" "$out/$module.lift.python" "$out/$module.lift.cpp" || status=1
done
exit "$status"
