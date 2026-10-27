# SPDX-License-Identifier: GPL-3.0-or-later
"""Select reproducible RomFS samples; the generated header contains names and hashes only."""
import json
import os
import random
import sys

manifest, folder, output = sys.argv[1:]
files = []
with open(manifest, encoding="utf-8") as f:
    for line in f:
        digest, sep, path = line.rstrip("\n").partition("  ")
        if sep and path.startswith("romfs/"):
            name = path[6:]
            files.append((name, digest, os.stat(os.path.join(folder, name)).st_size))
if len(files) != 41062:
    raise SystemExit("expected 41062 RomFS manifest entries")
files.sort(key=lambda item: (-item[2], item[0]))
largest = files[:20]
remainder = sorted(files[20:])
sample = random.Random(0x53575035).sample(remainder, 200)
with open(output, "w", encoding="utf-8") as f:
    f.write("// Generated from the host reference manifest; no key material.\n")
    f.write("static const Sample samples[] = {\n")
    for name, digest, size in largest + sample:
        f.write("    {%s, %s, %dULL},\n" % (json.dumps(name), json.dumps(digest), size))
    f.write("};\n")
