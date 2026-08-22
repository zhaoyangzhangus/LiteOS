#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -f kernel/core/display.c ]]; then
    echo "error: run from LiteOS repository root" >&2
    exit 2
fi
exec python3 "$SCRIPT_DIR/apply_compositor_p7.py" "$@"
