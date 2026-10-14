#!/usr/bin/env bash
set -euo pipefail

source_path="${1:-}"
if (($# != 1)) || [[ -z "$source_path" || ! -r "$source_path" ]]; then
    echo "usage: $0 PROCESS_MEMORY_DUMP_CC" >&2
    exit 2
fi

body="$(sed -n '/ProcessMemoryDump::CountResidentBytes(/,/^std::optional<size_t> ProcessMemoryDump::CountResidentBytesInSharedMemory/p' "$source_path")"
platform_branch="$(awk '
    /^[[:space:]]*#if defined\(__NATIVE_OS__\)[[:space:]]*$/ && !active { active=1; depth=1; next }
    active && /^[[:space:]]*#if/ { ++depth }
    active && /^[[:space:]]*#endif/ { if (--depth == 0) exit }
    active && depth == 1 && /^[[:space:]]*#(else|elif)/ { exit }
    active { print }
' <<<"$body")"
if [[ -z "$platform_branch" ]]; then
    echo "missing platform resident-memory fast path" >&2
    exit 1
fi
if ! grep -Eq '^[[:space:]]*return std::nullopt;[[:space:]]*$' <<<"$platform_branch"; then
    echo "Platform resident-memory path must return without probing /proc" >&2
    exit 1
fi

echo "Chrome memory profiler path: OK"
