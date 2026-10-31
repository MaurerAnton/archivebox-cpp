# ArchiveBox.cpp (`arcbox`)

[![CI](https://github.com/MaurerAnton/archivebox-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/MaurerAnton/archivebox-cpp/actions/workflows/ci.yml)

A small, self-hosted web archiver written in C++20. It captures pages with
libcurl, writes standards-compatible WARC files, extracts page metadata and
outgoing links into SQLite (with FTS5 search), and can run each capture inside a
`systemd-run` cgroup scope so a bad page cannot take the whole machine down.

This is an independent implementation inspired by
[ArchiveBox](https://github.com/ArchiveBox/ArchiveBox), not a line-by-line port
of its Django codebase. ArchiveBox is MIT-licensed; this project is MIT-licensed
as well. It is not affiliated with the ArchiveBox project.

**Status:** v0.3 alpha. HTTP/WARC capture, metadata, tags, URL import/export,
a read-only local web browser, queue priorities with retry, and opt-in headless
Chromium rendering (rendered DOM, screenshot, PDF) work. WACZ packaging,
faithful replay, and element-level ad blocking are not implemented yet.

## Why a C++ core?

The Python/Django ArchiveBox stack is excellent but heavy for an 8 GB machine.
A small C++ orchestrator avoids a Python runtime, a Django process, and a
Playwright dependency in the idle path. The real memory hog is still Chromium,
so this project treats browser capture as an external, disposable, cgrouped
worker rather than something the core process tries to host itself.

Target machine: 8 GB RAM, Arch Linux, systemd, cgroups v2.

## What works now

- `arcbox init` — create the data directory, SQLite index, and blocklist file.
- `arcbox add URL` — canonicalize and enqueue a URL.
- `arcbox capture ID|URL` — fetch one page with libcurl, write a gzipped WARC,
  save the raw body, response headers, extracted text, and metadata JSON.
- HTML metadata extraction: title, description, canonical URL, site name,
  author, published date, language, favicon, and outgoing links.
- `arcbox list` / `search` / `show`, with tag filters.
- `arcbox tag` / `untag` / `tags` — local, normalized tags.
- `arcbox import FILE|-` — queue a plain-text URL list.
- `arcbox export` — JSONL records or a plain URL list.
- `arcbox serve` — read-only local web UI with search, tag filters, detail
  pages, text previews, outgoing links, and raw-file downloads.
- `arcbox daemon [--once]` — process the queue sequentially.
- Host-level ad blocking before a request, using a hosts/ABP-style domain
  blocklist.
- Optional cgroup wrapping via `systemd-run --user --scope` (`MemoryHigh`,
  `MemoryMax`, `MemorySwapMax`, `CPUQuota`, `TasksMax`).
- WARC 1.0 records: `warcinfo`, `request`, `response`, gzip-compressed.
- Admission guard before every daemon capture: reads `/proc/meminfo` and
  `/proc/pressure/memory`, throttles when PSI pressure is high, and pauses the
  queue when free memory falls below ~1.5 GB.
- Headless Chromium rendering of the DOM, a screenshot and an optional PDF, opt-in
  and itself guarded, so JavaScript pages are archived without risking the host.
- Queue scheduling: `--priority N` decides what the daemon drains first, and a
  failed capture is retried with exponential backoff instead of being dropped.

## Not implemented yet

- Chromium/CDP rendering of JavaScript-heavy pages driven over WebSocket; today
  each artifact is produced by its own short-lived Chrome invocation via the
  command line, which is enough for the DOM, screenshot and PDF but cannot run
  scripts that need interaction.
- Element-level ad blocking (uBlock-style filter lists).
- WACZ packaging, content-addressed deduplication, and faithful replay.
- Adaptive scheduling beyond the current admission guard (size-aware admission,
  per-host pacing, recrawl policies, retention rules).

See [docs/ROADMAP.md](docs/ROADMAP.md) for the intended phases.

## Build (Arch Linux)

```bash
sudo pacman -S --needed base-devel cmake pkgconf curl sqlite openssl zlib
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j1
sudo cmake --install build
```

On a memory-constrained machine, use `-j1` for the build. The resulting
executable has no Python runtime dependency.

## Quick start

```bash
arcbox init

# Queue a page, then capture it later.
arcbox add 'https://example.com/article'
arcbox capture 1

# Or queue and capture in one go.
arcbox add --capture 'https://example.com/'

arcbox list
arcbox search example
arcbox show 1
arcbox tag 1 research
```

Import a URL list and browse the result:

```bash
printf '%s\n' 'https://example.com/' 'https://example.com/about' > urls.txt
arcbox import urls.txt --capture
arcbox export --tag research
arcbox serve
# open http://127.0.0.1:8080/
```

The default data directory is `~/.local/share/arcbox`. Override it with
`--data DIR` or `ARCBOX_DATA`:

```bash
arcbox --data /srv/arcbox add --capture 'https://example.com/'
```

## Local web UI

`arcbox serve` reads the SQLite index and serves a small, plain-HTML browser
plus the raw archive files. It binds to `127.0.0.1:8080` by default.

```bash
arcbox serve
arcbox serve --host 127.0.0.1 --port 8090
```

There is **no authentication**. Binding to anything other than localhost is
supported but discouraged; if you do it, put the service behind a trusted
reverse proxy. The UI is read-only: add, capture, tag, and import remain CLI
operations. Captured HTML is served as a download (`Content-Disposition:
attachment`), not rendered inline; the UI shows extracted text and links instead.

Routes:

```text
/                       index, FTS search (?q=...), tag filter (?tag=...)
/snapshot/ID            metadata, text preview, outgoing links
/snapshot/ID/text       extracted plain text
/snapshot/ID/body       raw response body (download)
/snapshot/ID/headers    response headers
/snapshot/ID/metadata   metadata JSON
/snapshot/ID/warc       gzipped WARC (download)
/healthz                health check
```

## Memory protection

Captures are attempted inside a transient systemd user scope by default when
`systemd-run` and a user bus are available:

```bash
arcbox capture 1
# equivalent to a scope with:
#   MemoryHigh=1200M
#   MemoryMax=1800M
#   MemorySwapMax=512M
#   CPUQuota=200%
#   TasksMax=300
```

If the user bus or `systemd-run` is unavailable, the capture runs directly.
Disable wrapping explicitly with `--no-cgroup` or `ARCBOX_NO_CGROUP=1`.

Limits can be changed with environment variables:

```bash
ARCBOX_MEMORY_HIGH=1G \
ARCBOX_MEMORY_MAX=1500M \
ARCBOX_MEMORY_SWAP_MAX=256M \
arcbox capture 1
```

For a persistent daemon, install the files in [`systemd/`](systemd/). They put
the daemon and its transient scopes into an `arcbox.slice` with a 2.5 GB ceiling.
This is a *ceiling*, not a reservation: the host remains protected, and capture
failures are treated as expendable.

### Admission guard

The cgroup caps one capture. The admission guard protects the *host*: before
every queue pop the daemon samples `/proc/meminfo` and `/proc/pressure/memory`
and either proceeds, throttles, or pauses. Thresholds are tunable:

```bash
ARCBOX_MIN_MEM_AVAILABLE_KB=1536000 \
ARCBOX_PSI_SOME_MAX=10 \
ARCBOX_THROTTLE_SLEEP=15 \
ARCBOX_PAUSE_SLEEP=60 \
arcbox daemon
```

### Headless rendering

Rendering is **off by default** (`ARCBOX_RENDER=1` enables it): it is the only
capture step that can cost hundreds of megabytes, so it stays opt-in, applies
only to successful HTML captures, and re-runs the admission guard right before
launching Chromium — when memory is tight the page is still archived and the
render is simply skipped with a note on stderr.

Each artifact comes from its own disposable browser process with a wall-clock
timeout and a size ceiling, so a page that hangs or explodes costs one process,
not the daemon:

```bash
ARCBOX_RENDER=1 \
ARCBOX_RENDER_PDF=1 \
ARCBOX_BROWSER=/path/to/chrome-headless-shell \
ARCBOX_RENDER_TIMEOUT=60 \
arcbox daemon
```

`ARCBOX_BROWSER` selects the binary; otherwise `chrome-headless-shell`,
`chromium`, or `google-chrome-stable` is used if found on `PATH`. No root
access is needed: an unpacked Chrome for Testing build works as-is.
Below the memory floor the queue pauses; above the PSI limit it throttles.
Both checks fail open: on a kernel without `CONFIG_PSI` only the memory floor
applies, and an unreadable `/proc/meminfo` cannot stall the queue forever. A
single `arcbox capture` bypasses the guard on purpose — it is already an
explicit request. With `--once` the daemon leaves the item queued instead of
sleeping, which makes the path testable.

The web UI itself holds one SQLite connection and renders one request at a time;
it does not spawn workers. Keep it in the same slice if you expose it.

**Note:** `/tmp` is often RAM-backed (tmpfs) on Arch. Do not put the archive
database or Chromium profile there unless you have checked `df -h /tmp`. The
default data path is on disk.

## Ad blocking

`arcbox init` creates `blocklist.txt`. One domain per line is accepted, plus
hosts-file and ABP host-rule forms:

```text
# plain domain
ads.example.com

# hosts file
0.0.0.0 tracker.example.net

# ABP host rule
||ads.example.org^

# exception (allow)
@@news.example.com
```

The blocklist is checked before the HTTP request. If the main URL's host is
blocked, the snapshot is marked `blocked` and no request is made. This is a
DNS/host-level guard, not a full in-page ad blocker, and it does not yet inspect
redirect targets. CDP request interception and filter-list support are planned.

## Data layout

```text
~/.local/share/arcbox/
├── index.sqlite3          # URLs, snapshots, metadata, tags, links, FTS5
├── blocklist.txt
├── warc/
│   └── 1-20261031T142431Z.warc.gz
├── snapshots/
│   └── 1/
│       ├── body.bin       # raw response body
│       ├── text.txt       # extracted text (HTML only)
│       ├── headers.txt    # response status line and headers
│       └── metadata.json  # capture metadata and extracted fields
└── objects/               # reserved for future CAS dedup
```

SQLite uses WAL mode. The queue is the set of `snapshots` rows with status
`queued`. A capture moves through `queued → running → ok|partial|failed|blocked`.
On daemon startup, rows left in `running` are recovered as `queued`.

## Commands

```text
arcbox init
arcbox add URL [--capture] [--priority N] [--no-block]
arcbox capture ID|URL [--no-cgroup] [--no-block]
arcbox list [--limit N] [--tag TAG]
arcbox search QUERY [--limit N] [--tag TAG]
arcbox show ID
arcbox tag ID TAG [TAG...]
arcbox untag ID TAG [TAG...]
arcbox tags
arcbox import FILE|- [--capture] [--no-cgroup] [--no-block]
arcbox export [--limit N] [--tag TAG] [--urls]
arcbox serve [--host HOST] [--port PORT]
arcbox daemon [--once] [--no-cgroup] [--no-block]
arcbox retry ID|all
arcbox version
```

`--no-block` bypasses the host blocklist for an explicit capture. Tags are
trimmed and lowercased; commas and control characters are rejected.

The daemon drains the queue highest priority first, oldest first. A failed
capture is not dropped: it is counted and requeued with exponential backoff
(`ARCBOX_MAX_ATTEMPTS`, default 5; `ARCBOX_RETRY_BACKOFF`, default 30 s, doubling
up to `ARCBOX_RETRY_BACKOFF_MAX`, default 1 h). A snapshot that used up all its
attempts stays `failed` until `arcbox retry` puts it back. A `blocked` decision
is policy, not an error, and is never retried.

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j1
ctest --test-dir build --output-on-failure
```

The smoke test starts local Python HTTP and arcbox servers, captures HTML with
metadata and links, checks the WARC, tags, import/export JSON, FTS5 search, the
local web routes, and security-relevant response behavior.

## License

MIT. See [LICENSE](LICENSE).
