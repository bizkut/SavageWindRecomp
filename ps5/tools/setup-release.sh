#!/usr/bin/env bash
# Linux PC generator environment, pinned to the versions used for this kit.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
python3 -m venv "$root/.venv"
"$root/.venv/bin/python" -m pip install 'capstone==5.0.9' 'unicorn==2.1.4' 'lz4==4.4.5'
echo "Run: source $root/.venv/bin/activate"
