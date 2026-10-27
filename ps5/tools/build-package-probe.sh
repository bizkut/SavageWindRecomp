#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the console probe; sample hashes come from the host reference manifest.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
manifest=${1:-/tmp/savagewind-import-nsp.sha256}
folder=${2:-"$root/build/romfs-reference/game/romfs"}
work="$root/build/ps5/importer"
bash "$root/ps5/tools/build-importer.sh"
python3 "$root/ps5/importer/probe_samples.py" "$manifest" "$folder" "$work/ps5-build/package_probe_samples.h"
sdk=$(bash "$root/ps5/tools/setup-deps.sh")
PS5_PAYLOAD_SDK="$sdk" cmake --build "$work/ps5-build" -j2 --target savagewind-package-probe
cp "$work/ps5-build/savagewind-package-probe" "$work/savagewind-package-probe.elf"
sha256sum "$work/savagewind-package-probe.elf"
