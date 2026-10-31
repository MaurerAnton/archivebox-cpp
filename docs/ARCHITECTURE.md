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

## Current capture path

1. `add` canonicalizes the URL and inserts a queued snapshot.
2. `capture`/`daemon` marks the snapshot `running`.
3. The host blocklist is checked. A blocked main URL is recorded without a
   network request.
4. libcurl fetches with redirects, timeouts, a body-size ceiling, and
   `http,https` protocol restrictions.
5. The response is written as a gzipped WARC with `warcinfo`, `request`, and
   `response` records.
6. The raw body and HTML-extracted text are stored under the snapshot directory.
7. SHA-256 is recorded; text is inserted into FTS5.
8. The snapshot moves to `ok`, `partial`, `failed`, or `blocked`.

## Storage

- SQLite in WAL mode is the source of truth for queue state and metadata.
- `warc/` contains WARC 1.0 gzip files.
- `snapshots/<id>/` contains raw body and extracted text for replay/search.
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
- A small localhost-only replay/search UI.
