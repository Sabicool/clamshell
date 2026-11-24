#!/usr/bin/env bash
# Runs Anki's own sync server (from the official `anki` Python package) on this
# Mac so desktop Anki, AnkiDroid and the 3DS can all sync to it over Wi-Fi.
#
#   tools/run-sync-server.sh            # asks for a username/password the first time
#
# Data lives in ./server-data (git-ignored). Stop with Ctrl-C.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$ROOT/tools/.venv"
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q anki==26.08
fi
CRED_FILE="$ROOT/server-data/.credentials"
mkdir -p "$ROOT/server-data"
if [ ! -f "$CRED_FILE" ]; then
  read -r -p "Choose a sync username: " u
  read -r -s -p "Choose a sync password: " p; echo
  printf '%s:%s\n' "$u" "$p" > "$CRED_FILE"
  chmod 600 "$CRED_FILE"
fi
IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo 'this-mac-ip')"
PORT="${SYNC_PORT:-8080}"
echo "Sync server: http://$IP:$PORT/   (use this URL in desktop Anki and on the 3DS)"
SYNC_USER1="$(cat "$CRED_FILE")" SYNC_BASE="$ROOT/server-data" SYNC_HOST=0.0.0.0 SYNC_PORT="$PORT" \
  exec "$VENV/bin/python" -m anki.syncserver
