#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
tools/gen_cpp/check_sites.sh --tables-only
python3 tools/gen_cpp/generate_patches.py --check
make -C tools/gen_cpp ../../build/s9-cpp/gen
root=$(mktemp -d build/s9-cpp/slice5.XXXXXXXX)
exefs=${1:-build/exefs-1.9.0}
py="$root/python"
cpp="$root/cpp"
mkdir -p "$py/main" "$cpp/main" "$py/all" "$cpp/all"
compare() {
    python3 - "$1" "$2" "$3" <<'PY'
import pathlib, sys
name, left, right = sys.argv[1:]
a, b = pathlib.Path(left), pathlib.Path(right)
af = {p.relative_to(a).as_posix() for p in a.rglob('*') if p.is_file()}
bf = {p.relative_to(b).as_posix() for p in b.rglob('*') if p.is_file()}
print(f'{name}: Python {len(af)} files, C++ {len(bf)} files')
if af != bf:
    print('Python-only:', sorted(af - bf))
    print('C++-only:', sorted(bf - af))
    sys.exit(1)
for f in sorted(af):
    x, y = (a / f).read_bytes(), (b / f).read_bytes()
    if x != y:
        for line, (l, r) in enumerate(zip(x.splitlines(keepends=True), y.splitlines(keepends=True)), 1):
            if l != r:
                print(f'{name}: first difference {f}:{line}\nPython: {l[:500]!r}\nC++: {r[:500]!r}')
                break
        else:
            print(f'{name}: first difference {f}: line count or trailing newline')
        sys.exit(1)
print(f'{name}: identical ({len(af)} files)')
PY
}
/usr/bin/time -f 'main Python: %e s, peak RSS %M KiB' tools/venv/bin/python tools/gen.py \
    main "$exefs/main" generated-ps5/layout.txt "$py/main" --hle runtime/hle/hle_symbols.txt --simd c
/usr/bin/time -f 'main C++: %e s, peak RSS %M KiB' build/s9-cpp/gen \
    main "$exefs/main" generated-ps5/layout.txt "$cpp/main" --hle runtime/hle/hle_symbols.txt --simd c
compare main "$py/main" "$cpp/main"
/usr/bin/time -f 'all Python: %e s, peak RSS %M KiB' tools/venv/bin/python tools/gen.py \
    all "$exefs" "$py/all" --hle runtime/hle/hle_symbols.txt --simd c
/usr/bin/time -f 'all C++: %e s, peak RSS %M KiB' build/s9-cpp/gen \
    all "$exefs" "$cpp/all" --hle runtime/hle/hle_symbols.txt --simd c
compare all "$py/all" "$cpp/all"
