#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-$HOME/LiteOS}"
DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$DIR/apply_compositor_v2.py" "$ROOT"
