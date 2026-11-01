#!/usr/bin/env bash
# Real-host part of "verify behavior on 8 GB with zram".
#
# Unlike tests/memory.sh this does not fake /proc: it reads the machine it runs
# on, applies a small, quickly released ballast to create genuine pressure, and
# checks that the guard crosses from proceed to pause and back. It therefore
# only does the interesting part when the host actually is in the 8 GB class
# and has room for the ballast; everywhere else it reports and exits 0, so it
# is safe in CI.
#
# Nothing here can make arcbox itself an OOM source: arcbox only reads /proc,
# and the ballast is a plain allocation that is freed again immediately.
set -euo pipefail

BIN="${1:?usage: bigmemory.sh /path/to/arcbox}"
TMP="$(mktemp -d)"
BALLAST_PID=""
cleanup() {
    if [[ -n "$BALLAST_PID" ]]; then
        kill "$BALLAST_PID" 2>/dev/null || true
        wait "$BALLAST_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

fail() {
    echo "big memory check failed: $*" >&2
    exit 1
}

effective_line() {
    "$BIN" memcheck | awk '$1 == "Effective" {print $2}' | tr -d 'M'
}

mem_total_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
mem_avail_kb="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
swap_total_kb="$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)"

echo "--- host profile ---"
echo "MemTotal     $((mem_total_kb / 1024))M"
echo "MemAvailable $((mem_avail_kb / 1024))M"
echo "SwapTotal    $((swap_total_kb / 1024))M"
if grep -q '^/dev/zram' /proc/swaps 2>/dev/null; then
    echo "SwapBackend  zram"
else
    echo "SwapBackend  $(awk 'NR>1 {print $1}' /proc/swaps 2>/dev/null | tr '\n' ' ')"
fi
"$BIN" memcheck

# Below ~7 GB the guard is not being verified against the profile the roadmap
# asks about; say so instead of pretending.
if [[ $mem_total_kb -lt 7000000 ]]; then
    echo
    echo "skipped: MemTotal $((mem_total_kb / 1024))M is below the ~8 GB class"
    echo "this check is about; nothing was verified here."
    exit 0
fi
# Ballast sizing: take at most a third of the headroom above 1.2G and never
# more than 300M, so the check adapts to whatever the host can spare instead of
# carrying a constant tuned for one machine.
headroom_kb=$((mem_avail_kb - 1200 * 1024))
if [[ $headroom_kb -lt 256000 ]]; then
    echo
    echo "skipped: only $((mem_avail_kb / 1024))M available, too little headroom"
    echo "for a safe ballast here."
    exit 0
fi
BALLAST_MB=$((headroom_kb / 3 / 1024))
[[ $BALLAST_MB -le 300 ]] || BALLAST_MB=300
[[ $BALLAST_MB -ge 64 ]] || BALLAST_MB=64
if ! command -v python3 >/dev/null 2>&1; then
    echo
    echo "skipped: python3 not available for the ballast."
    exit 0
fi

# --- real pressure ---------------------------------------------------------
echo
echo "--- real pressure: allocating ${BALLAST_MB}M ---"
python3 -c "
import time
ballast = bytearray(${BALLAST_MB} * 1024 * 1024)
for offset in range(0, len(ballast), 4096):
    ballast[offset] = 1
open('$TMP/ready', 'w').write('1')
time.sleep(120)
" &
BALLAST_PID=$!
for _ in $(seq 1 60); do
    [[ -s $TMP/ready ]] && break
    sleep 0.5
done
[[ -s $TMP/ready ]] || fail "ballast did not come up"

rest_effective=0
pressured_effective="$(effective_line)"
echo "effective while pressured: ${pressured_effective}M"
if [[ -z $pressured_effective || $pressured_effective -le 0 ]]; then
    fail "memcheck did not report an effective value"
fi

# A floor above the pressured reading must pause; one below it must not. The
# comparison uses whatever the host reports right now, so no absolute numbers
# are baked in.
pause_floor=$(( (pressured_effective + 120) * 1024 ))
proceed_floor=$(( (pressured_effective - 300) * 1024 ))
[[ $proceed_floor -gt 0 ]] || proceed_floor=1024

pause_out="$(ARCBOX_MIN_MEM_AVAILABLE_KB="$pause_floor" "$BIN" memcheck)"
grep -Fq "Admission     pause" <<<"$pause_out" \
    || fail "guard did not pause at floor $((pause_floor / 1024))M while pressured"
echo "ok   pause at floor $((pause_floor / 1024))M (effective ${pressured_effective}M)"

proceed_out="$(ARCBOX_MIN_MEM_AVAILABLE_KB="$proceed_floor" "$BIN" memcheck)"
grep -Fq "Admission     proceed" <<<"$proceed_out" \
    || fail "guard did not proceed at floor $((proceed_floor / 1024))M while pressured"
echo "ok   proceed at floor $((proceed_floor / 1024))M"

# The same pair of decisions has to reach the queue, not just the reporter.
"$BIN" --data "$TMP/data" init >/dev/null
"$BIN" --data "$TMP/data" add 'http://127.0.0.1:9/' >/dev/null
queue_out="$(ARCBOX_MIN_MEM_AVAILABLE_KB="$pause_floor" \
    "$BIN" --data "$TMP/data" daemon --once --no-cgroup 2>&1)"
grep -Fq "admission pause" <<<"$queue_out" \
    || fail "daemon did not pause: $queue_out"
"$BIN" --data "$TMP/data" list | grep -Fq "queued" \
    || fail "snapshot left the queue while the host was pressured"
echo "ok   daemon defers the capture and leaves it queued"

kill "$BALLAST_PID" 2>/dev/null || true
wait "$BALLAST_PID" 2>/dev/null || true
BALLAST_PID=""
sleep 3

released_effective="$(effective_line)"
echo "effective after release: ${released_effective}M"
if [[ $released_effective -le $pressured_effective ]]; then
    fail "effective memory did not recover after the ballast was freed"
fi
released_out="$(ARCBOX_MIN_MEM_AVAILABLE_KB="$pause_floor" "$BIN" memcheck)"
grep -Fq "Admission     proceed" <<<"$released_out" \
    || fail "guard stayed paused at floor $((pause_floor / 1024))M after release"
echo "ok   proceeds again once the memory is back"
echo
echo "real-host check finished; keep this output as the machine's evidence."
