#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/s9-cpp
python3 tools/gen_cpp/generate_patches.py --check
${CXX:-c++} -std=c++17 -Wall -Wextra -O2 tools/gen_cpp/patches.cpp tools/gen_cpp/patches_check.cpp \
    -o build/s9-cpp/patches_check
tools/venv/bin/python tools/gen_cpp/check_patches.py "${1:-build/exefs-1.9.0/main}"
