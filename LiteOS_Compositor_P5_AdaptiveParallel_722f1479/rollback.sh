#!/usr/bin/env bash
set -euo pipefail

TARGET="kernel/graphics/window_server.c"
BACKUP="${TARGET}.before-compositor-p5"

if [[ ! -f "$BACKUP" ]]; then
  echo "error: missing backup: $BACKUP" >&2
  exit 2
fi

cp -f -- "$BACKUP" "$TARGET"
echo "restored: $TARGET"
