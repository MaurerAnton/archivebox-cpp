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
- [ ] Element-level ad blocking inside the browser (needs CDP interception).

## v0.4 — memory-aware scheduling

The admission guard already shipped early, as part of v0.2:

- [x] Read `/proc/meminfo` and `/proc/pressure/memory`.
- [x] Pause queue when `MemAvailable < 1.5G` or PSI `some avg10 > 10`.
- [x] Retry failed captures with exponential backoff and drain by priority
      instead of first-come-first-served; attempts survive restarts.
- [ ] Verify behavior on 8 GB with zram.

## v0.5 — ad blocking and replay

- [ ] CDP Fetch interception with filter-list rules.
- [ ] Import hosts/ABP lists; update command.
- [ ] Per-snapshot block decision log.
- [ ] Localhost-only search/replay UI (already shipped in v0.2).
- [ ] WACZ export.

## v0.6 — dedup and operations

- [ ] Content-addressed storage for bodies/WARCs.
- [ ] Scheduled recrawls and retention policy.
- [ ] Browser extension / share target (optional).
- [ ] Packaging for Arch (`PKGBUILD`) and a systemd user unit.

## Non-goals

- Reimplementing every ArchiveBox extractor.
- Running a full Django/Playwright stack inside the C++ core.
- Making the archiver more important than host responsiveness.
