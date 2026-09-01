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

loader_program_headers="$($objdump -p "$loader")"
library_program_headers="$($objdump -p "$library")"
test_program_headers="$($objdump -p "$test_image")"
test_interpreter="$($objdump -s -j .interp "$test_image")"
library_symbols="$($objdump -T "$library")"
test_relocations="$($objdump -R "$test_image")"

if grep -Fq 'NEEDED' <<<"$loader_program_headers" ||
   grep -Fq 'NEEDED' <<<"$library_program_headers"; then
    printf 'dynamic loader: loader or graphics library has an external dependency\n' >&2
    exit 1
fi
if ! grep -Fq 'INTERP' <<<"$test_program_headers" ||
   ! grep -Fq '2f6c6962' <<<"$test_interpreter" ||
   ! grep -Fq '6c697465' <<<"$test_interpreter" ||
   ! grep -Fq '6f732e73' <<<"$test_interpreter" ||
   ! grep -Fq '6f2e3100' <<<"$test_interpreter"; then
    printf 'dynamic loader: test image has no LiteOS interpreter\n' >&2
    exit 1
fi
if ! grep -Fq 'NEEDED' <<<"$test_program_headers" ||
   ! grep -Fq 'libliteosgfx.so.1' <<<"$test_program_headers"; then
    printf 'dynamic loader: test image has no graphics-library dependency\n' >&2
    exit 1
fi
for symbol in liteos_gfx_clear liteos_gfx_fill_rect \
              liteos_gfx_gradient_rect liteos_gfx_frame; do
    if ! grep -Fq "$symbol" <<<"$library_symbols"; then
        printf 'dynamic loader: missing exported graphics symbol: %s\n' "$symbol" >&2
        exit 1
    fi
done
if ! grep -Fq 'JUMP_SLOT' <<<"$test_relocations"; then
    printf 'dynamic loader: test image has no dynamic call relocations\n' >&2
    exit 1
fi

printf 'dynamic loader sanity passed: PT_INTERP, DT_NEEDED, exports, and JUMP_SLOT\n'
