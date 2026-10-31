#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: smoke.sh /path/to/arcbox}"
DATA="$(mktemp -d)"
SITE="$(mktemp -d)"
HTTP_PID=""
WEB_PID=""

cleanup() {
    for pid in "$WEB_PID" "$HTTP_PID"; do
        if [[ -n "$pid" ]]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$DATA" "$SITE"
}
trap cleanup EXIT

fail() {
    echo "smoke test failed: $*" >&2
    exit 1
}

assert_contains() {
    local haystack="$1"
    local needle="$2"
    local label="$3"
    if ! grep -Fq -- "$needle" <<<"$haystack"; then
        echo "expected '$needle' in $label" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required for the smoke test" >&2
    exit 1
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required for the smoke test" >&2
    exit 1
fi
if ! command -v gzip >/dev/null 2>&1; then
    echo "gzip is required for the smoke test" >&2
    exit 1
fi

cat >"$SITE/index.html" <<'HTML'
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>Arcbox smoke test</title>
    <meta name="description" content="Smoke test page for ArchiveBox.cpp">
    <meta property="og:site_name" content="Smoke Site">
    <meta name="author" content="Arcbox Test">
    <meta property="article:published_time" content="2026-10-31T12:00:00Z">
    <link rel="canonical" href="/canonical">
    <link rel="icon" href="/favicon.ico">
  </head>
  <body>
    <script>window.secretValue = "must-not-be-indexed";</script>
    <h1>Hello archive</h1>
    <p>This page tests WARC capture and full text search.</p>
    <a href="/second.html">Second page</a>
    <a href="https://example.com/path?x=1&amp;y=2">External &amp; link</a>
  </body>
</html>
HTML

cat >"$SITE/second.html" <<'HTML'
<!doctype html>
<html lang="en">
  <head><title>Second smoke page</title></head>
  <body><h1>Imported snapshot</h1><p>This page was queued from a URL list.</p></body>
</html>
HTML

free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

PORT="$(free_port)"
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

run_arcbox() {
    "$BIN" --data "$DATA" "$@"
}

run_arcbox init >/dev/null
run_arcbox add "http://127.0.0.1:$PORT/" >/dev/null

CAPTURE_OUT="$(run_arcbox capture --no-cgroup --id 1)"
assert_contains "$CAPTURE_OUT" "#1 ok 200" "capture output"

SHOW_OUT="$(run_arcbox show 1)"
assert_contains "$SHOW_OUT" "title:       Arcbox smoke test" "show output"
assert_contains "$SHOW_OUT" "description: Smoke test page for ArchiveBox.cpp" "show output"
assert_contains "$SHOW_OUT" "canonical:   http://127.0.0.1:$PORT/canonical" "show output"
assert_contains "$SHOW_OUT" "site:        Smoke Site" "show output"
assert_contains "$SHOW_OUT" "author:      Arcbox Test" "show output"
assert_contains "$SHOW_OUT" "language:    en" "show output"
assert_contains "$SHOW_OUT" "metadata:" "show output"
assert_contains "$SHOW_OUT" "headers:" "show output"

test -s "$DATA/snapshots/1/text.txt" || fail "extracted text is missing"
assert_contains "$(cat "$DATA/snapshots/1/text.txt")" "Hello archive" "text.txt"
if grep -Fq "must-not-be-indexed" "$DATA/snapshots/1/text.txt"; then
    fail "script content leaked into extracted text"
fi
test -s "$DATA/snapshots/1/headers.txt" || fail "headers.txt is missing"
assert_contains "$(cat "$DATA/snapshots/1/headers.txt")" "Content-type:" "headers.txt"

python3 - "$DATA/snapshots/1/metadata.json" "$PORT" <<'PY'
import json
import sys

path, port = sys.argv[1], sys.argv[2]
with open(path, encoding="utf-8") as handle:
    metadata = json.load(handle)
assert metadata["http_status"] == 200
assert metadata["content_type"] == "text/html"
assert metadata["extracted"]["title"] == "Arcbox smoke test"
assert metadata["extracted"]["canonical_url"] == f"http://127.0.0.1:{port}/canonical"
assert metadata["extracted"]["links_count"] == 2
PY

WARC="$(find "$DATA/warc" -name '*.warc.gz' -type f | head -n 1)"
[[ -n "$WARC" ]] || fail "WARC file was not created"
gzip -dc "$WARC" | grep -Fq "WARC-Type: response" || fail "WARC response record is missing"

cat >"$DATA/import.txt" <<EOF
# comments and non-HTTP URLs are skipped
ftp://example.com/not-http
http://127.0.0.1:$PORT/second.html
EOF
IMPORT_OUT="$(run_arcbox import "$DATA/import.txt" --capture --no-cgroup)"
assert_contains "$IMPORT_OUT" "imported 1, skipped 1, capture failures 0" "import output"
assert_contains "$IMPORT_OUT" "#2 ok 200" "import capture output"

run_arcbox tag 1 smoke research >/dev/null
run_arcbox tag 2 imported >/dev/null
TAGS_OUT="$(run_arcbox tags)"
assert_contains "$TAGS_OUT" "imported	1" "tags output"
assert_contains "$TAGS_OUT" "smoke	1" "tags output"

run_arcbox untag 1 research >/dev/null
SHOW_TAGS="$(run_arcbox show 1)"
assert_contains "$SHOW_TAGS" "tags:        smoke" "show tags"

TAGGED_LIST="$(run_arcbox list --tag smoke)"
assert_contains "$TAGGED_LIST" "http://127.0.0.1:$PORT/" "tagged list"
if grep -Fq "http://127.0.0.1:$PORT/second.html" <<<"$TAGGED_LIST"; then
    fail "tag filter included an untagged snapshot"
fi

SEARCH_OUT="$(run_arcbox search "full text")"
assert_contains "$SEARCH_OUT" "Arcbox smoke test" "search output"
IMPORTED_SEARCH="$(run_arcbox search imported --tag imported)"
assert_contains "$IMPORTED_SEARCH" "Second smoke page" "tagged search output"
EMPTY_SEARCH="$(run_arcbox search imported --tag missing)"
assert_contains "$EMPTY_SEARCH" "no matches" "empty tagged search"

EXPORT_JSON="$(run_arcbox export --tag smoke)"
python3 - "$EXPORT_JSON" "$PORT" <<'PY'
import json
import sys

record = json.loads(sys.argv[1])
port = sys.argv[2]
assert record["title"] == "Arcbox smoke test"
assert record["canonical_url"] == f"http://127.0.0.1:{port}/canonical"
assert record["tags"] == ["smoke"]
assert record["metadata_path"] == "snapshots/1/metadata.json"
PY

EXPORT_URLS="$(run_arcbox export --urls)"
assert_contains "$EXPORT_URLS" "http://127.0.0.1:$PORT/" "URL export"
assert_contains "$EXPORT_URLS" "http://127.0.0.1:$PORT/second.html" "URL export"

WEB_PORT="$(free_port)"
"$BIN" --data "$DATA" serve --host 127.0.0.1 --port "$WEB_PORT" \
    >"$DATA/web.log" 2>&1 &
WEB_PID=$!

WEB_READY=0
for _ in $(seq 1 50); do
    if curl -fsS "http://127.0.0.1:$WEB_PORT/healthz" >/dev/null 2>&1; then
        WEB_READY=1
        break
    fi
    if ! kill -0 "$WEB_PID" 2>/dev/null; then
        echo "web server exited early" >&2
        cat "$DATA/web.log" >&2
        exit 1
    fi
    sleep 0.2
done
if [[ "$WEB_READY" -ne 1 ]]; then
    echo "web server did not become ready" >&2
    cat "$DATA/web.log" >&2
    exit 1
fi

WEB_BASE="http://127.0.0.1:$WEB_PORT"
INDEX_HTML="$(curl -fsS "$WEB_BASE/")"
assert_contains "$INDEX_HTML" "Local archive" "web index"
assert_contains "$INDEX_HTML" "Arcbox smoke test" "web index"
assert_contains "$INDEX_HTML" "href=\"/snapshot/1\"" "web index"

WEB_HEADERS="$(curl -fsSI "$WEB_BASE/")"
assert_contains "$WEB_HEADERS" "Content-Security-Policy:" "web response headers"

FILTERED_HTML="$(curl -fsS "$WEB_BASE/?tag=smoke")"
assert_contains "$FILTERED_HTML" "tag active" "web tag filter"
SEARCH_HTML="$(curl -fsS "$WEB_BASE/?q=imported")"
assert_contains "$SEARCH_HTML" "Second smoke page" "web search"
assert_contains "$SEARCH_HTML" "value=\"imported\"" "web search form"

DETAIL_HTML="$(curl -fsS "$WEB_BASE/snapshot/1")"
assert_contains "$DETAIL_HTML" "Canonical URL" "web detail"
assert_contains "$DETAIL_HTML" "Outgoing links" "web detail"
assert_contains "$DETAIL_HTML" "metadata JSON" "web detail"
assert_contains "$DETAIL_HTML" "Hello archive" "web detail"

TEXT_BODY="$(curl -fsS "$WEB_BASE/snapshot/1/text")"
assert_contains "$TEXT_BODY" "Hello archive" "web text route"
HEADERS_BODY="$(curl -fsS "$WEB_BASE/snapshot/1/headers")"
assert_contains "$HEADERS_BODY" "Content-type:" "web headers route"
METADATA_BODY="$(curl -fsS "$WEB_BASE/snapshot/1/metadata")"
assert_contains "$METADATA_BODY" '"canonical_url"' "web metadata route"

curl -fsS "$WEB_BASE/snapshot/1/warc" -o "$DATA/web.warc.gz"
gzip -dc "$DATA/web.warc.gz" | grep -Fq "WARC-Type: response" \
    || fail "web WARC route returned an invalid WARC"

HEAD_STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -I "$WEB_BASE/snapshot/1/body")"
[[ "$HEAD_STATUS" == "200" ]] || fail "HEAD /snapshot/1/body returned $HEAD_STATUS"
MISSING_STATUS="$(curl -sS -o /dev/null -w '%{http_code}' "$WEB_BASE/snapshot/999")"
[[ "$MISSING_STATUS" == "404" ]] || fail "missing snapshot returned $MISSING_STATUS"
POST_STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$WEB_BASE/")"
[[ "$POST_STATUS" == "405" ]] || fail "POST / returned $POST_STATUS"

# --- PSI-aware admission guard ---
ADM_DATA="$DATA/admission"
"$BIN" --data "$ADM_DATA" init >/dev/null
"$BIN" --data "$ADM_DATA" add "http://127.0.0.1:$PORT/" >/dev/null

# An unreachable memory floor must defer the capture instead of running it.
ADM_OUT="$(ARCBOX_MIN_MEM_AVAILABLE_KB=999999999 \
    "$BIN" --data "$ADM_DATA" daemon --once --no-cgroup 2>&1)"
assert_contains "$ADM_OUT" "admission pause" "guard pauses on low memory"
assert_contains "$ADM_OUT" "leaving queued" "guard leaves snapshot queued"
"$BIN" --data "$ADM_DATA" list | grep -Fq "queued" \
    || fail "snapshot should stay queued under memory pressure"

# With the floor effectively removed the same queue must go through.
ARCBOX_MIN_MEM_AVAILABLE_KB=1 "$BIN" --data "$ADM_DATA" daemon --once --no-cgroup \
    >"$DATA/admission-proceed.log" 2>&1
if grep -Fq "admission pause" "$DATA/admission-proceed.log"; then
    fail "guard still paused with a 1 kB floor"
fi
"$BIN" --data "$ADM_DATA" list | grep -Fq "queued" \
    && fail "snapshot should have been attempted with a 1 kB floor"

kill "$WEB_PID" 2>/dev/null || true
wait "$WEB_PID" 2>/dev/null || true
WEB_PID=""

kill "$HTTP_PID" 2>/dev/null || true
wait "$HTTP_PID" 2>/dev/null || true
HTTP_PID=""

echo "smoke test passed"
