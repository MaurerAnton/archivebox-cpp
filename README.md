# ArchiveBox.cpp (`arcbox`)

[![CI](https://github.com/MaurerAnton/archivebox-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/MaurerAnton/archivebox-cpp/actions/workflows/ci.yml)

A small, self-hosted web archiver written in C++20. It captures pages with
libcurl, writes standards-compatible WARC files, indexes extracted text in
SQLite FTS5, and can run each capture inside a `systemd-run` cgroup scope so a
bad page cannot take the whole machine down.

This is an independent implementation inspired by
[ArchiveBox](https://github.com/ArchiveBox/ArchiveBox), not a line-by-line port
of its Django codebase. ArchiveBox is MIT-licensed; this project is MIT-licensed
as well. It is not affiliated with the ArchiveBox project.

**Status:** early bootstrap. The CLI and HTTP/WARC pipeline work. Chromium
rendering, PDF/screenshot capture, WACZ export, and the web UI are not
implemented yet.

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
  save the raw body and extracted text.
- `arcbox list` / `search` / `show` — inspect the local archive and FTS5 index.
- `arcbox daemon [--once]` — process the queue sequentially.
- Host-level ad blocking before a request, using a hosts/ABP-style domain
  blocklist.
- Optional cgroup wrapping via `systemd-run --user --scope` (`MemoryHigh`,
  `MemoryMax`, `MemorySwapMax`, `CPUQuota`, `TasksMax`).
- WARC 1.0 records: `warcinfo`, `request`, `response`, gzip-compressed.

## Not implemented yet

- Chromium/CDP rendering and JavaScript-heavy pages.
- PDF, screenshot, and `monolith` single-file capture.
- Element-level ad blocking (uBlock-style filter lists).
- WACZ packaging, CAS deduplication, replay UI, browser extension.
- PSI-based adaptive scheduling and automatic recrawl policies.

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
```

The default data directory is `~/.local/share/arcbox`. Override it with
`--data DIR` or `ARCBOX_DATA`:

```bash
arcbox --data /srv/arcbox add --capture 'https://example.com/'
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
DNS/host-level guard, not a full in-page ad blocker. CDP request interception
and filter-list support are planned.

## Data layout

```text
~/.local/share/arcbox/
├── index.sqlite3          # URLs, snapshots, queue state, FTS5 index
├── blocklist.txt
├── warc/
│   └── 1-20261031T142431Z.warc.gz
├── snapshots/
│   └── 1/
│       ├── body.bin       # raw response body
│       └── text.txt       # extracted text (HTML only)
└── objects/               # reserved for future CAS dedup
```

SQLite uses WAL mode. The queue is the set of `snapshots` rows with status
`queued`. A capture moves through `queued → running → ok|partial|failed|blocked`.
On daemon startup, rows left in `running` are recovered as `queued`.

## Commands

```text
arcbox init
arcbox add URL [--capture] [--no-block]
arcbox capture ID|URL [--no-cgroup] [--no-block]
arcbox list [--limit N]
arcbox search QUERY [--limit N]
arcbox show ID
arcbox daemon [--once] [--no-cgroup] [--no-block]
arcbox version
```

`--no-block` bypasses the host blocklist for an explicit capture.

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j1
ctest --test-dir build --output-on-failure
```

The smoke test starts a local Python HTTP server, captures a page, checks the
WARC, and verifies FTS5 search.

## License

MIT. See [LICENSE](LICENSE).
