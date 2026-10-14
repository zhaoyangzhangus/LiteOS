#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 3 ]]; then
    printf 'usage: %s LOADER LIBRARY TEST\n' "$0" >&2
    exit 2
fi

loader="$1"
library="$2"
test_image="$3"
objdump="${ELFOBJDUMP:-${ELFTOOLPREFIX:-x86_64-elf-}objdump}"

for image in "$loader" "$library" "$test_image"; do
    if [[ ! -f "$image" ]]; then
        printf 'dynamic loader: missing ELF: %s\n' "$image" >&2
        exit 1
    fi
done
if ! command -v "$objdump" >/dev/null 2>&1; then
    printf 'dynamic loader: missing inspection tool: %s\n' "$objdump" >&2
    exit 1
fi

python3 "$(dirname -- "$0")/verify-elf-runtime.py" "$loader" "$library" "$test_image" libgraphics.so.1
library_symbols="$($objdump -T "$library")"
test_relocations="$($objdump -R "$test_image")"

for symbol in gfx_clear gfx_fill_rect \
              gfx_gradient_rect gfx_frame; do
    if ! awk -v symbol="$symbol" '$NF == symbol && $0 !~ /\*UND\*/ {found=1} END {exit !found}' <<<"$library_symbols"; then
        printf 'dynamic loader: missing exported graphics symbol: %s\n' "$symbol" >&2
        exit 1
    fi
done
if ! grep -Fq 'JUMP_SLOT' <<<"$test_relocations"; then
    printf 'dynamic loader: test image has no dynamic call relocations\n' >&2
    exit 1
fi

printf 'dynamic loader sanity passed: PT_INTERP, DT_NEEDED, exports, and JUMP_SLOT\n'
