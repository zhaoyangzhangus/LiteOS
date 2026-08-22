#!/usr/bin/env bash
set -euo pipefail
files=(
  "kernel/core/display.c"
  "OS_Implementation_Specification_COMPLETE/include/kernel/display.h"
)
for file in "${files[@]}"; do
    backup="${file}.before-compositor-p7"
    if [[ ! -f "$backup" ]]; then
        echo "error: missing backup: $backup" >&2
        exit 2
    fi
done
for file in "${files[@]}"; do
    cp -f -- "${file}.before-compositor-p7" "$file"
    echo "restored: $file"
done
