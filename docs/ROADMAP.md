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

## v0.2 — browser capture

- [ ] Chromium via CDP, one render worker at a time.
- [ ] Save rendered HTML and screenshot.
- [ ] Optional PDF via Chromium; optional `monolith`.
- [ ] Browser process recycling every 10–20 captures.
- [ ] Render timeout and body/screenshot size ceilings.
- [ ] Degradation ladder when memory is tight.

## v0.3 — memory-aware scheduling

- [ ] Read `/proc/meminfo` and `/proc/pressure/memory`.
- [ ] Pause queue when `MemAvailable < 1.5G` or PSI `some avg10 > 10`.
- [ ] Queue priority and retry/backoff.
- [ ] Verify behavior on 8 GB with zram.

## v0.4 — ad blocking and replay

- [ ] CDP Fetch interception with filter-list rules.
- [ ] Import hosts/ABP lists; update command.
- [ ] Per-snapshot block decision log.
- [ ] Localhost-only search/replay UI.
- [ ] WACZ export.

## v0.5 — dedup and operations

- [ ] Content-addressed storage for bodies/WARCs.
- [ ] Scheduled recrawls and retention policy.
- [ ] Import/export metadata.
- [ ] Browser extension / share target (optional).
- [ ] Packaging for Arch (`PKGBUILD`) and a systemd user unit.

## Non-goals

- Reimplementing every ArchiveBox extractor.
- Running a full Django/Playwright stack inside the C++ core.
- Making the archiver more important than host responsiveness.
