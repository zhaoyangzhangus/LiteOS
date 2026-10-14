#!/usr/bin/env bash
set -euo pipefail

if (($# != 2)) || [[ ! -r "$1" || ! -r "$2" ]]; then
    echo "usage: $0 dependency_manager.cc syscall.c" >&2
    exit 2
fi
source_file=$1
libc_source=$2

if ! grep -Fq '#if defined(__NATIVE_OS__) && defined(OS_STARTUP_TRACE)' "$source_file"; then
    echo "Chrome keyed-service tracing is not behind an explicit startup trace flag" >&2
    exit 1
fi

if ! grep -Fq '(void)phase;' "$source_file" ||
   ! grep -Fq '(void)name;' "$source_file"; then
    echo "Chrome keyed-service trace disabled path is missing" >&2
    exit 1
fi

if ! grep -Fq 'OS_RUNTIME_TRACE' "$libc_source" ||
   ! grep -Fq 'g_libc_service_trace_drop_chunks' "$libc_source"; then
    echo "Runtime diagnostics are not gated and keyed-service output is not filtered" >&2
    exit 1
fi

echo "Chrome runtime trace gate: OK"
