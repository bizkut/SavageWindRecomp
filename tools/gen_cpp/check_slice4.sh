#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
make -C tools/gen_cpp ../../build/s9-cpp/gen
for module in subsdk0 sdk; do
    py="build/s9-cpp/slice4-python/$module"
    cpp="build/s9-cpp/slice4-cpp/$module"
    mkdir -p "$py" "$cpp"
    find "$py" "$cpp" -maxdepth 1 -type f -name '*.c' -delete
    /usr/bin/time -f "$module Python: %e s, peak RSS %M KiB" tools/venv/bin/python tools/gen.py \
        "$module" "build/exefs-1.9.0/$module" generated-ps5/layout.txt "$py" \
        --hle runtime/hle/hle_symbols.txt --simd c
    /usr/bin/time -f "$module C++: %e s, peak RSS %M KiB" build/s9-cpp/gen \
        "$module" "build/exefs-1.9.0/$module" generated-ps5/layout.txt "$cpp" \
        --hle runtime/hle/hle_symbols.txt --simd c
    python3 - "$module" "$py" "$cpp" <<'PY'
import pathlib, sys
name, pa, pb = sys.argv[1:]
a, b = pathlib.Path(pa), pathlib.Path(pb)
af = {p.relative_to(a).as_posix() for p in a.rglob('*') if p.is_file()}
bf = {p.relative_to(b).as_posix() for p in b.rglob('*') if p.is_file()}
print(f'{name}: Python {len(af)} files, C++ {len(bf)} files')
if af != bf:
    print('Python-only:', sorted(af - bf)); print('C++-only:', sorted(bf - af)); sys.exit(1)
for f in sorted(af):
    x, y = (a/f).read_bytes(), (b/f).read_bytes()
    if x != y:
        for n, (l, r) in enumerate(zip(x.splitlines(keepends=True), y.splitlines(keepends=True)), 1):
            if l != r:
                print(f'{name}: first difference {f}:{n}\nPython: {l[:500]!r}\nC++:    {r[:500]!r}'); break
        else: print(f'{name}: first difference {f}: line count or trailing newline')
        sys.exit(1)
print(f'{name}: identical ({len(af)} files)')
PY
done
