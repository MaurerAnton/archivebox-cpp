# Verifying the guard on ~8 GB with zram

The memory guard is only useful if its thresholds match a real machine. This
page records what "8 GB with zram" means for arcbox, how the numbers are
computed, and what has actually been measured instead of assumed.

## The rule

```
effective_available = MemAvailable + SwapFree * ARCBOX_ZRAM_SWAP_WEIGHT
```

* `ARCBOX_ZRAM_SWAP_WEIGHT` defaults to `0.5`. zram holds compressed pages, so
  a megabyte of zram swap is worth roughly half a megabyte of RAM.
* The credit applies **only** when a swap device is backed by zram. Disk-backed
  swap (a swapfile or a partition) is deliberately worth zero: disk swap keeps
  the box alive but makes captures unusably slow, and it must not convince the
  guard to start another browser. The backend is read from `/proc/swaps`.
* Everything else is unchanged: below the floor the queue pauses
  (`ARCBOX_MIN_MEM_AVAILABLE_KB`, default 1.5G), above it PSI decides whether
  to throttle (`ARCBOX_PSI_SOME_MAX`, default `some avg10 > 10`).
* The same `effective_available` picks the render tier, so with zram a host can
  afford a heavier artifact than `MemAvailable` alone suggests.

## Reading what the host reports

```bash
arcbox memcheck
```

reports `MemTotal`, `MemAvailable`, swap with its backend, the effective value,
the floor, PSI, and the resulting admission decision. For scripting or
containers, the sources can be pointed elsewhere: `ARCBOX_MEMINFO_FILE`,
`ARCBOX_SWAPS_FILE`, `ARCBOX_PSI_FILE`.

## Tests

| test | what it does | when it is meaningful |
| --- | --- | --- |
| `tests/memory.sh` | replays 8 GB + zram profiles through fake `/proc` files and cross-checks `memcheck` against the real `/proc` | everywhere |
| `tests/bigmemory.sh` | allocates an adaptive ballast (at most 300M) on the real host and checks the proceed → pause → proceed crossing | ~8 GB hosts with enough headroom; skips itself otherwise |

Both run under `ctest`; the second one exits 0 (and says so) on smaller hosts
rather than faking a result.

## Preparing an Arch box

`zram-generator` is the packaged way to get zram swap:

```ini
# /etc/systemd/zram-generator.conf
[zram0]
zram-size = min(ram / 2, 4096)
compression-algorithm = zstd
```

```bash
sudo systemctl daemon-reload
sudo systemctl start /dev/zram0
swapon --show        # /dev/zram0 should be listed
arcbox memcheck      # Swap ... (zram)
```

Then run the real check and keep its output:

```bash
ctest --test-dir build --output-on-failure -R bigmemory
```

## Measured (2026-11-01)

Host: 7.7 GB RAM (`MemTotal 7866920 kB`), one zram device of 3 GB already
~95% used, `MemAvailable` hovering around 2.0G. That is a stressed but real
member of the "8 GB with zram" class, not a simulation.

| state | MemAvailable | zram free | effective | admission at default floor (1.5G) |
| --- | --- | --- | --- | --- |
| idle | 1969M | 133M | 2036M | proceed |
| ~265M ballast | 1712M | 136M | 1794M | proceed |
| ~265M ballast, floor 1914M | 1717M | 136M | 1786M | pause, snapshot stays queued |
| ballast released | 1988M | 136M | 2040M | proceed |

With the ballast in place the daemon printed

```
arcbox: admission pause: effective available 1786M below floor 1855M
        (MemAvailable 1717M plus zram swap 136M); --once, leaving queued
```

and the snapshot was still `queued` afterwards; freeing the ballast restored
`proceed` and the capture was attempted.

Reproduce with:

```bash
ctest --test-dir build --output-on-failure -R bigmemory
# or directly
bash tests/bigmemory.sh ./build/arcbox
```

## Not measured

* A fresh 8 GB box with mostly idle zram: on such a machine `effective_available`
  is dominated by `MemAvailable` anyway, so the zram credit rarely decides
  anything until RAM is nearly full — exactly what the profiles in
  `tests/memory.sh` cover.
* Behaviour under disk swap alone: intentionally credited as zero, so the guard
  is expected to pause earlier there. No run available for that combination yet.
