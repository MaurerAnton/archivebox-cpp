# Roadmap

## v0.1 — bootstrap (current)

- [x] CMake/C++20 CLI.
- [x] SQLite WAL schema and queue.
- [x] libcurl HTTP fetch with limits and redirects.
- [x] gzipped WARC 1.0 output (`warcinfo`, `request`, `response`).
- [x] HTML text extraction and FTS5 search.
- [x] Host blocklist.
- [x] Optional `systemd-run` cgroup scope.
- [x] Smoke test with a local HTTP server.

## v0.2 — metadata, tags, import/export, local web UI (done)

- [x] URL/HTML helpers and metadata extraction (title, description, Open Graph, JSON-LD, favicon, canonical).
- [x] Expanded DB schema: snapshots gain `tags`, `links`, `metadata_json`; new `tags` and `links` tables with FTS5.
- [x] Capture writes `headers.txt` and `metadata.json` alongside `body.html` and `text.txt`.
- [x] Commands: `tag`, `untag`, `tags`, `import` (JSONL/CSV), `export` (JSONL/CSV/WARC index).
- [x] Local web UI: `serve` command with index, search, detail, raw-file routes, `/healthz`, CSP headers.
- [x] Smoke test extended for metadata, tags, import/export, and web server.

## v0.3 — browser capture

- [x] Chromium rendering behind a memory guard; HTML-only and opt-in, one
      short-lived process per artifact instead of a persistent render worker.
- [x] Save rendered DOM (`rendered.html`) and screenshot (`screenshot.png`).
- [x] Optional PDF via `--print-to-pdf` (`ARCBOX_RENDER_PDF=1`); `monolith`
      still not implemented.
- [x] Every artifact gets a fresh process, so recycling is inherent.
- [x] Render timeout and artifact size ceilings.
- [x] Admission guard applies to the render step as well; the page is archived
      even when the render is deferred.
- [x] Degradation ladder: DOM-only → DOM+screenshot → DOM+screenshot+PDF by
      free memory; a `Throttle` verdict drops one tier instead of stopping.
- [x] Size-aware render admission: a source above
      `ARCBOX_RENDER_MAX_SOURCE_BYTES` is archived without a browser.
- [x] Element-level ad blocking inside the browser (needs CDP interception):
      the interception pass injects a script that removes `iframe`/`img`/
      `script`/`link` elements pointing at blocked hosts; cosmetic filter
      rules remain unimplemented.
- [x] Network-level ad blocking inside the browser: the blocklist becomes
      `--host-resolver-rules` per render pass (capped, page host and exceptions
      skipped).
- [x] `arcbox blocklist check URL` and `arcbox blocklist import FILE` for ABP
      and hosts-file lists.

## v0.4 — memory-aware scheduling

The admission guard already shipped early, as part of v0.2:

- [x] Read `/proc/meminfo` and `/proc/pressure/memory`.
- [x] Pause queue when `MemAvailable < 1.5G` or PSI `some avg10 > 10`.
- [x] Retry failed captures with exponential backoff and drain by priority
      instead of first-come-first-served; attempts survive restarts.
- [ ] Verify behavior on 8 GB with zram.

## v0.5 — ad blocking and replay

- [x] CDP Fetch interception with filter-list rules (`arcbox render` via
      DevTools: every request is answered through `Fetch.requestPaused`,
      blocked ones fail before leaving the browser; one WebSocket session per
      page, bounded messages, classic path as fallback,
      `ARCBOX_RENDER_INTERCEPT=0` to opt out).
- [x] Import hosts/ABP lists (`arcbox blocklist import`); fetching and updating
      a remote list on a schedule is still open.
- [x] Per-snapshot block decision log (`snapshots/<id>/blocked.txt`: stage,
      URL, and the blocklist entry that decided it, written both for the HTTP
      fetch and for the requests the browser asked for through interception;
      counted in `arcbox show`).
- [x] Localhost-only search/replay UI (`arcbox serve`, shipped in v0.2: reads
      the SQLite index, serves search plus the raw archive files, binds
      127.0.0.1 by default).
- [x] WACZ export (`arcbox wacz FILE`): datapackage.json, pages/pages.jsonl
      and the existing gzipped WARC members, streamed in 64 KiB pieces so an
      export cannot be an OOM source; zip64-size archives are refused.

## v0.6 — dedup and operations

- [x] Content-addressed storage for bodies (bodies live once under
      `objects/<prefix>/<sha256>`, each snapshot hard-links its own
      `body.bin` to it, so identical captures share the bytes and a removed
      snapshot cannot orphan its siblings; `arcbox prune` reclaims objects
      that nothing links any more). WARCs stay per-snapshot.
- [x] Scheduled recrawls and retention policy (`arcbox recrawl`, `arcbox prune`
      --keep/--older-than/--dry-run); daemon-side scheduling is still manual.
- [x] Browser extension / share target (`POST /add` on the local UI: a `url`
      field or free text whose first link is queued, `GET /add` as a form, a
      bookmarklet covers the browser side; 64 KiB request cap, no auth, refused
      with 400 when the share holds no link). A real extension package is not
      planned.
- [ ] Packaging for Arch (`PKGBUILD`) and a systemd user unit.

## Non-goals

- Reimplementing every ArchiveBox extractor.
- Running a full Django/Playwright stack inside the C++ core.
- Making the archiver more important than host responsiveness.
