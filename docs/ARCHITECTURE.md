# Architecture

## Goal

ArchiveBox.cpp is a low-idle-memory orchestrator for a personal web archive.
It does not try to reimplement every ArchiveBox extractor in C++. Instead it
owns the queue, metadata, storage, and scheduling, while delegating heavy
capture work to external binaries and disposable workers.

## Process model

```text
arcbox CLI / daemon
  └── SQLite queue + metadata + FTS5
        └── capture worker (one at a time)
              ├── libcurl HTTP fetch -> WARC + body
              └── future: Chromium via CDP -> HTML/screenshot/PDF
                          (launched in systemd-run --scope)
```

Only one HTTP/browser capture runs at a time by default. Browser profiles are
not stored in tmpfs, and browser processes are recycled between captures.
The core process is intentionally small; Chromium is the dominant memory user.

## Current capture path (v0.2)

1. `add` canonicalizes the URL and inserts a queued snapshot.
2. `capture`/`daemon` marks the snapshot `running`.
3. The host blocklist is checked. A blocked main URL is recorded without a
   network request.
4. libcurl fetches with redirects, timeouts, a body-size ceiling, and
   `http,https` protocol restrictions.
5. The response is written as a gzipped WARC with `warcinfo`, `request`, and
   `response` records.
6. The raw body, HTML-extracted text, **response headers**, and **extracted
   metadata** (title, description, Open Graph, JSON-LD, favicon, canonical URL)
   are stored under the snapshot directory as `body.html`, `text.txt`,
   `headers.txt`, and `metadata.json`.
7. SHA-256 is recorded; text is inserted into FTS5.
8. The snapshot moves to `ok`, `partial`, `failed`, or `blocked`.

## Storage (v0.2)

- SQLite in WAL mode is the source of truth for queue state and metadata.
- `snapshots` table extended: `tags` (TEXT), `links` (TEXT), `metadata_json` (TEXT).
- New `tags` table: `snapshot_id`, `tag` with FTS5 on `tag`.
- New `links` table: `snapshot_id`, `url`, `rel`, `text` with FTS5 on `url`, `text`.
- `warc/` contains WARC 1.0 gzip files.
- `snapshots/<id>/` contains `body.html`, `text.txt`, `headers.txt`, `metadata.json`.
- `objects/` is reserved for content-addressed deduplication. The current
  bootstrap stores one body per snapshot; dedup is a later optimization.

## Memory and host protection

Everything that can grow is pushed outside the core process:

- HTTP body limit: 100 MiB by default (`ARCBOX_MAX_BODY_BYTES`).
- Capture timeout: 60 seconds; connect timeout: 15 seconds.
- One capture at a time in the daemon.
- Default transient scope: `MemoryHigh=1200M`, `MemoryMax=1800M`,
  `MemorySwapMax=512M`, `CPUQuota=200%`, `TasksMax=300`.
- Persistent daemon units use `arcbox.slice` with `MemoryMax=2500M`.

The host is treated as more important than the archiver. A capture that
exceeds its cgroup limit is killed by the kernel; the queue row is marked
failed, and the daemon continues.

## Local web UI (v0.2)

- `arcbox serve` starts a CivetWeb-based HTTP server on `127.0.0.1:8080`.
- Routes:
  - `/` — paginated index with optional `?q=fts5&tag=foo&status=ok`.
  - `/search` — same as index, rendered for HTMX/fetch.
  - `/snapshots/<id>` — detail page with metadata, tags, links, extracted text.
  - `/raw/<id>/<file>` — serves `body.html`, `text.txt`, `headers.txt`, `metadata.json`, `output.warc.gz` with correct MIME.
  - `/healthz` — JSON `{status,version,db_path}`.
- CSP headers restrict scripts/styles to self; no external resources.
- Signal handling (SIGINT/SIGTERM) for graceful shutdown.

## Why not put Django/Playwright in the core?

The original ArchiveBox is ~112k lines of Python/Django and relies on
Playwright/Chromium. Running that on 8 GB alongside editing and agents causes
memory pressure even when idle. This project keeps the idle path small and
makes browser work an explicit, capped scope. It can still reuse external
tools (`chromium`, `monolith`, `pdftotext`, `yt-dlp`, `gallery-dl`) without
reimplementing them.

## Next architectural steps

- CDP worker with one browser, one page, and a render budget.
- PSI-aware admission: pause capture when `MemAvailable < 1.5G` or
  `some avg10 > 10`.
- Degradation ladder: full capture → HTML/screenshot only → WARC only →
  metadata only.
- Content-addressed body dedup and WACZ packaging.
