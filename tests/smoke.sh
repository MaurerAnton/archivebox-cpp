#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: smoke.sh /path/to/arcbox}"
DATA="$(mktemp -d)"
SITE="$(mktemp -d)"
HTTP_PID=""

cleanup() {
    if [[ -n "$HTTP_PID" ]]; then
        kill "$HTTP_PID" 2>/dev/null || true
        wait "$HTTP_PID" 2>/dev/null || true
    fi
    rm -rf "$DATA" "$SITE"
}
trap cleanup EXIT

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required for the smoke test" >&2
    exit 1
fi

cat >"$SITE/index.html" <<'HTML'
<!doctype html>
<html>
  <head><title>Arcbox smoke test</title></head>
  <body>
    <h1>Hello archive</h1>
    <p>This page tests WARC capture and full text search.</p>
  </body>
</html>
HTML

PORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$SITE" \
    >"$DATA/http.log" 2>&1 &
HTTP_PID=$!

READY=0
for _ in $(seq 1 50); do
    if curl -fsS "http://127.0.0.1:$PORT/" >/dev/null 2>&1; then
        READY=1
        break
    fi
    if ! kill -0 "$HTTP_PID" 2>/dev/null; then
        echo "local HTTP server exited early" >&2
        cat "$DATA/http.log" >&2
        exit 1
    fi
    sleep 0.2
done
if [[ "$READY" -ne 1 ]]; then
    echo "local HTTP server did not become ready" >&2
    cat "$DATA/http.log" >&2
    exit 1
fi

"$BIN" --data "$DATA" init >/dev/null
"$BIN" --data "$DATA" add "http://127.0.0.1:$PORT/" >/dev/null
"$BIN" --data "$DATA" capture --no-cgroup --id 1 >/dev/null

"$BIN" --data "$DATA" list | grep -q "http://127.0.0.1:$PORT/"
"$BIN" --data "$DATA" search archive | grep -q "Arcbox smoke test"

WARC="$(find "$DATA/warc" -name '*.warc.gz' -type f | head -n 1)"
if [[ -z "$WARC" ]]; then
    echo "WARC file was not created" >&2
    exit 1
fi
gzip -dc "$WARC" | grep -q "WARC-Type: response"
test -s "$DATA/snapshots/1/text.txt"

echo "smoke test passed"
