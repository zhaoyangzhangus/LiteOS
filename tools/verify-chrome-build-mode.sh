#!/usr/bin/env bash
set -euo pipefail

args_file="${1:-}"
if [[ -z "$args_file" || ! -f "$args_file" ]]; then
    echo "usage: $0 /path/to/args.gn" >&2
    exit 2
fi

if grep -Eq '^[[:space:]]*is_debug[[:space:]]*=[[:space:]]*true([[:space:]]|$)' "$args_file"; then
    echo "Chrome native runtime build must not use is_debug=true (-O0)" >&2
    exit 1
fi

for setting in symbol_level blink_symbol_level v8_symbol_level; do
    if ! grep -Eq "^[[:space:]]*${setting}[[:space:]]*=[[:space:]]*0([[:space:]]|$)" "$args_file"; then
        echo "Chrome native runtime build must set ${setting}=0" >&2
        exit 1
    fi
done

echo "Chrome native runtime build mode: optimized, no symbols"
