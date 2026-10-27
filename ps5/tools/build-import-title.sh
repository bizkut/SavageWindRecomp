#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Embed the exact importer supplied by the caller, then use the normal title flow.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
[[ $# == 1 && -f $1 ]] || { echo "usage: $0 <savagewind-import.elf>" >&2; exit 2; }
elf=$(realpath -- "$1")
[[ $elf != *'"'* && $elf != *$'\n'* && $elf != *'\'* ]] || { echo 'unsupported ELF path' >&2; exit 2; }
mkdir -p "$root/build/ps5/import-title"
asm="$root/build/ps5/import-title/import-elf.S"
printf '.section .rodata\n.global import_elf_start\n.global import_elf_end\nimport_elf_start:\n.incbin "%s"\nimport_elf_end:\n.section .note.GNU-stack,"",@progbits\n' "$elf" > "$asm"
bash "$root/ps5/tools/build-title.sh" import-title ps5/title/import_ui.c ps5/title/import_status.c ps5/title/display.c build/ps5/import-title/import-elf.S
