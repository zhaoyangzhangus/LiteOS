#!/usr/bin/env bash
set -euo pipefail

source_file=${1:?usage: verify-qemu-runtime-mode.sh run-qemu.sh}

if ! grep -Fq 'serial_trace=0' "$source_file"; then
    echo "QEMU runtime mode must default to serial tracing disabled" >&2
    exit 1
fi

if ! grep -Fq -- '--serial-trace' "$source_file"; then
    echo "QEMU runtime mode is missing the explicit serial trace switch" >&2
    exit 1
fi

if ! grep -Fq 'OS_DEBUG_SERIAL="$serial_trace"' "$source_file"; then
    echo "QEMU build mode does not propagate the serial trace choice" >&2
    exit 1
fi

echo "QEMU runtime mode: OK"
