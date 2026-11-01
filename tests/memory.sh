#!/usr/bin/env bash
# Admission guard checks for big-memory hosts.
#
# Two halves:
#   1. The real host: report what the guard sees here and cross-check it
#      against /proc, so a wrong reading fails instead of being believed.
#   2. Simulated 8 GB + zram hosts: /proc/meminfo, /proc/swaps and
#      /proc/pressure/memory are overridden through the environment, so the
#      interesting profiles can be exercised on any machine (see
#      docs/bigmemory.md).
#
# The simulated half is a model, not a measurement: it proves the guard reads
# swap and PSI the way the docs claim. Real behaviour on a real 8 GB box is
# covered by docs/bigmemory.md.
set -euo pipefail

BIN="${1:?usage: memory.sh /path/to/arcbox}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() {
    echo "memory test failed: $*" >&2
    exit 1
}

expect_admission() {
    local want="$1"
    local label="$2"
    shift 2
    local out
    out="$("$@" memcheck 2>&1)"
    if ! grep -Fq "Admission     $want" <<<"$out"; then
        echo "expected admission '$want' for $label" >&2
        printf '%s\n' "$out" >&2
        exit 1
    fi
    printf 'ok   %-46s %s\n' "$label" "$want"
}

# A meminfo file with the given MemTotal/MemAvailable/SwapFree, in kB.
meminfo() {
    local total_mb="$1"
    local avail_mb="$2"
    local swap_free_mb="$3"
    printf 'MemTotal:       %d kB\nMemAvailable:   %d kB\nSwapTotal:      %d kB\nSwapFree:       %d kB\n' \
        "$((total_mb * 1024))" "$((avail_mb * 1024))" "$((swap_free_mb * 1024))" \
        "$((swap_free_mb * 1024))" >"$TMP/meminfo"
}

swaps() {
    printf 'Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n/dev/%s partition\t4194304\t0\t100\n' "$1" >"$TMP/swaps"
}

psi() {
    printf 'some avg10=%s avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n' "$1" >"$TMP/psi"
}

# --- real host -------------------------------------------------------------
echo "--- host profile ---"
mem_total_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
mem_avail_kb="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
swap_total_kb="$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)"
echo "MemTotal $((mem_total_kb / 1024))M, MemAvailable $((mem_avail_kb / 1024))M, swap $((swap_total_kb / 1024))M"
if grep -q '^/dev/zram' /proc/swaps 2>/dev/null; then
    echo "swap backend: zram"
else
    echo "swap backend: $(awk 'NR>1 {print $1}' /proc/swaps 2>/dev/null | tr '\n' ' ')"
fi

HOST_OUT="$("$BIN" memcheck)"
printf '%s\n' "$HOST_OUT"
# MemAvailable moves under the test's feet (the box has zram and is actively
# swapping), so these are tolerance checks, not equality ones.
extract_mb() {
    # extract_mb <first-field> <field-with-MB-value> ; reads the memcheck report
    awk -v key="$1" -v idx="$2" '$1 == key {
        value = $idx
        sub(/M$/, "", value)
        printf "%d\n", value
    }' <<<"$HOST_OUT"
}

abs_diff() {
    local a="$1"
    local b="$2"
    if [[ $a -gt $b ]]; then
        echo $((a - b))
    else
        echo $((b - a))
    fi
}

# Values are reported in whole MB and MemAvailable moves under the test's feet
# (the box has zram and is actively swapping), so these are tolerance checks
# rather than equality ones.
for pair in "MemTotal 2 $mem_total_kb 1" "MemAvailable 2 $mem_avail_kb 256" "Swap 5 $swap_total_kb 1"; do
    read -r key idx proc_kb tolerance <<<"$pair"
    reported="$(extract_mb "$key" "$idx")"
    proc_mb=$((proc_kb / 1024))
    [[ $(abs_diff "$reported" "$proc_mb") -le $tolerance ]] \
        || fail "memcheck $key ${reported}M disagrees with /proc/meminfo ${proc_mb}M"
done

# Cross-check the credit rule against the host itself: with a floor above any
# plausible effective value the guard must pause, whatever the host looks like.
expect_admission pause "host: unreachable floor pauses" \
    env ARCBOX_MIN_MEM_AVAILABLE_KB=999999999 "$BIN"
expect_admission proceed "host: zero floor proceeds" \
    env ARCBOX_MIN_MEM_AVAILABLE_KB=1 "$BIN"

# --- simulated 8 GB + zram profiles ---------------------------------------
echo "--- 8 GB box with 4 GB zram ---"
export ARCBOX_MEMINFO_FILE="$TMP/meminfo"
export ARCBOX_SWAPS_FILE="$TMP/swaps"
export ARCBOX_PSI_FILE="$TMP/psi"

swaps zram0
psi 0.00

# Comfortable box: plenty of RAM, cold pages compressed on zram.
meminfo 8192 4096 2048
expect_admission proceed "healthy 8 GB box" "$BIN"

# Same box after a burst: RAM is nearly gone but zram still absorbs 1.5 GB.
# Half of that is credited, so the guard still lets the queue run.
meminfo 8192 1024 1536
expect_admission proceed "low RAM, zram absorbs it" "$BIN"

# Below the floor even with the zram credit: pause instead of rendering.
meminfo 8192 512 1536
expect_admission pause "RAM and zram both tight" "$BIN"

# The knob is what decides between the two cases above.
meminfo 8192 512 1536
expect_admission proceed "same state, full swap credit" \
    env ARCBOX_ZRAM_SWAP_WEIGHT=1 "$BIN"
expect_admission pause "same state, swap ignored" \
    env ARCBOX_ZRAM_SWAP_WEIGHT=0 "$BIN"

# Disk swap must never make the guard optimistic: same numbers, different
# backend, so the effective value stays at MemAvailable.
swaps sda2
meminfo 8192 1024 1536
expect_admission pause "disk swap is not credited" "$BIN"
swaps zram0

# PSI still throttles independently of how much memory is free.
meminfo 8192 4096 2048
psi 25.0
expect_admission throttle "PSI some 25 throttles" "$BIN"
psi 10.0
expect_admission proceed "PSI at the limit proceeds" "$BIN"
psi 10.5
expect_admission throttle "PSI just above the limit throttles" "$BIN"

echo "all admission profiles behave as documented"
