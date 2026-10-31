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

- [ ] Chromium via CDP, one render worker at a time.
- [ ] Save rendered HTML and screenshot.
- [ ] Optional PDF via Chromium; optional `monolith`.
- [ ] Browser process recycling every 10–20 captures.
- [ ] Render timeout and body/screenshot size ceilings.
- [ ] Degradation ladder when memory is tight.

## v0.4 — memory-aware scheduling

- [ ] Read `/proc/meminfo` and `/proc/pressure/memory`.
- [ ] Pause queue when `MemAvailable < 1.5G` or PSI `some avg10 > 10`.
- [ ] Queue priority and retry/backoff.
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
