#!/usr/bin/env bash
# tools/bench.sh — Phosphoric throughput benchmark (sprint 36a).
#
# Runs the emulator headless under `--bench` for 3 representative
# workloads and reports cycles/sec, MHz-equivalent, and speed ratio
# vs real ORIC hardware (1 MHz target).
#
# Output is human-readable; TSV mode for tracking via `--tsv`.

set -u
cd "$(dirname "$0")/.." || exit 1

EMU=./oric1-emu
BASIC_ROM=roms/basic11b.rom
DISK_ROM=roms/microdis.rom
LOCI_ROM=roms/loci/locirom
DSK=disks/SEDO40u.DSK
SDIMG=loci_demo.img
TAPE=tapes/007.tap

CYCLES_DEFAULT=${CYCLES:-100000000}   # 100M cycles ≈ 100 s of emulated ORIC
TSV_MODE=0
for arg in "$@"; do
    [ "$arg" = "--tsv" ] && TSV_MODE=1
done

if [ ! -x "$EMU" ]; then
    echo "ERROR: $EMU not built (run \`make SDL2=1\` or \`make\`)" >&2
    exit 1
fi

# ── Helpers ───────────────────────────────────────────────────────
run_bench() {
    local label="$1"; shift
    local out
    out=$("$EMU" "$@" --bench -c "$CYCLES_DEFAULT" 2>/dev/null \
          | grep -F "BENCH " | head -n 1)
    if [ -z "$out" ]; then
        echo "FAIL  ${label}: no BENCH line"
        return 1
    fi
    # Parse `BENCH cycles=N frames=N wall_ms=X mhz_eq=Y speed_ratio=Zx frame_us=W`
    local cycles wall_ms mhz_eq speed frame_us
    cycles=$(awk -F'cycles=' '{print $2}' <<<"$out" | awk '{print $1}')
    wall_ms=$(awk -F'wall_ms=' '{print $2}' <<<"$out" | awk '{print $1}')
    mhz_eq=$(awk -F'mhz_eq=' '{print $2}' <<<"$out" | awk '{print $1}')
    speed=$(awk -F'speed_ratio=' '{print $2}' <<<"$out" | awk '{print $1}')
    frame_us=$(awk -F'frame_us=' '{print $2}' <<<"$out" | awk '{print $1}')
    if [ "$TSV_MODE" = 1 ]; then
        printf "%s\t%s\t%s\t%s\t%s\t%s\n" \
            "$label" "$cycles" "$wall_ms" "$mhz_eq" "$speed" "$frame_us"
    else
        printf "  %-30s cycles=%-12s wall=%6sms  %5.5sMHz  %s  frame=%6sus\n" \
            "$label" "$cycles" "$wall_ms" "$mhz_eq" "$speed" "$frame_us"
    fi
}

skip_if_missing() {
    for f in "$@"; do
        if [ ! -e "$f" ]; then
            echo "  SKIP $1 (missing $f)"
            return 1
        fi
    done
    return 0
}

# ── Header ───────────────────────────────────────────────────────
if [ "$TSV_MODE" = 1 ]; then
    printf "label\tcycles\twall_ms\tmhz_eq\tspeed_ratio\tframe_us\n"
else
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Phosphoric throughput bench (sprint 36a)"
    echo "  Target : $CYCLES_DEFAULT cycles per scenario (≈ $((CYCLES_DEFAULT / 1000000)) s of emulated ORIC)"
    echo "  Real ORIC : 1 MHz (50 Hz PAL). mhz_eq > 1.0 means faster than real time."
    echo "═══════════════════════════════════════════════════════════════"
fi

# ── Scenarios ────────────────────────────────────────────────────
# 1. BASIC idle — lightest workload (no I/O activity).
if skip_if_missing "$BASIC_ROM"; then
    run_bench "basic-idle" -r "$BASIC_ROM"
fi

# 2. Sedoric V4 boot via Microdisc native — heavy realistic workload
#    (FDC + video + audio + Sedoric ROM exec).
if skip_if_missing "$BASIC_ROM" "$DISK_ROM" "$DSK"; then
    run_bench "sedoric-native" -r "$BASIC_ROM" --disk-rom "$DISK_ROM" -d "$DSK"
fi

# 3. Sedoric V4 boot via LOCI — same workload via the LOCI MIA bridge,
#    measures the LOCI overhead.
if skip_if_missing "$LOCI_ROM" "$SDIMG"; then
    run_bench "sedoric-loci" -r "$LOCI_ROM" --loci --loci-sdimg "$SDIMG" \
        --type-keys '15000000:\p3a\p2 \p2 \p2\e'
fi

# 4. 007 game loading via fast tape — TAP processing + game intro
#    animation (PSG + HIRES heavy).
if skip_if_missing "$BASIC_ROM" "$TAPE"; then
    run_bench "tape-007-fastload" -r "$BASIC_ROM" -t "$TAPE" -f
fi

if [ "$TSV_MODE" != 1 ]; then
    echo "═══════════════════════════════════════════════════════════════"
fi
