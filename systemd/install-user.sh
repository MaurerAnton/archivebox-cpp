#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-$ROOT/build/arcbox}"
UNIT_DIR="$HOME/.config/systemd/user"

if [[ ! -x "$BIN" ]]; then
    echo "binary not found: $BIN" >&2
    echo "build first, then pass the path: $0 /path/to/arcbox" >&2
    exit 1
fi

install -Dm755 "$BIN" "$HOME/.local/bin/arcbox"
mkdir -p "$UNIT_DIR"
install -m644 "$ROOT/systemd/arcbox.slice" "$UNIT_DIR/arcbox.slice"
install -m644 "$ROOT/systemd/arcbox.service" "$UNIT_DIR/arcbox.service"

systemctl --user daemon-reload
echo "installed units to $UNIT_DIR"
echo "start with: systemctl --user enable --now arcbox.service"
echo "keep running after logout: loginctl enable-linger $USER"
echo "inspect with: systemctl --user status arcbox.service"
