#!/usr/bin/env bash
# tools/bench_check.sh — BLOCKING performance budget (V2-E7, US7.1).
#
# `make bench` measures; this script DECIDES. An emulated PAL frame has
# 20 ms of real time; V2 commits to the emulation consuming at most
# 5 % of it (1000 µs) on the reference machine — leaving headroom for rendering,
# sound and peripherals, and a clear alarm if a "per-cycle" epic
# makes the cost explode.
#
# Measurement: the lightest scenario (BASIC boot, headless), which isolates the
# CPU core + master clock + ULA + VIA + PSG with no disk I/O. The figure
# tracked from sprint to sprint is that frame_us (491 → 521 → 555 → 611 µs).
#
# Usage: tools/bench_check.sh                  (budget 1000 µs, 20 M cycles)
#         BENCH_BUDGET_US=1500 tools/bench_check.sh   (slow CI: raise it)
#         BENCH_CYCLES=100000000 tools/bench_check.sh
# Exit: 0 if under budget, 1 otherwise, 0 + SKIP if the ROM or the binary is missing.

set -u
cd "$(dirname "$0")/.." || exit 1

EMU=./oric1-emu
ROM=roms/basic11b.rom
[ -f "$ROM" ] || ROM=roms/basic10.rom
BUDGET=${BENCH_BUDGET_US:-1000}
CYCLES=${BENCH_CYCLES:-20000000}
RUNS=${BENCH_RUNS:-3}

echo "=== Budget de performance (V2-E7, US7.1) : ≤ ${BUDGET} µs/trame ==="
[ -x "$EMU" ] || { echo "  SKIP: $EMU non construit"; exit 0; }
[ -f "$ROM" ] || { echo "  SKIP: aucune ROM BASIC"; exit 0; }

# A throttled machine (laptop on battery, "low-power" profile, collapsed
# frequency) measures not the emulator but its own power saving: the
# same binary costs 1040 µs there versus 601 µs at full speed. In that case we
# measure and DISPLAY, but do not decide (SKIP, not FAIL). BENCH_STRICT=1
# forces the verdict anyway (CI, reference machine).
throttled=""
prof=$(cat /sys/firmware/acpi/platform_profile 2>/dev/null)
[ "$prof" = "low-power" ] && throttled="profil d'énergie « low-power »"
for st in /sys/class/power_supply/BAT*/status; do
    [ -f "$st" ] && [ "$(cat "$st")" = "Discharging" ] && throttled="sur batterie"
done
cur=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)
max=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null)
if [ -n "$cur" ] && [ -n "$max" ] && [ "$cur" -lt $((max / 2)) ]; then
    throttled="fréquence CPU $((cur / 1000)) MHz sur $((max / 1000)) MHz"
fi
[ "${BENCH_STRICT:-0}" = 1 ] && throttled=""

# Best of RUNS measurements: we judge the emulator, not the machine's load.
best=""
for i in $(seq 1 "$RUNS"); do
    out=$("$EMU" -r "$ROM" -n --bench -c "$CYCLES" 2>/dev/null | grep -F "BENCH " | head -n 1)
    us=$(sed -n 's/.*frame_us=\([0-9.]*\).*/\1/p' <<<"$out")
    if [ -z "$us" ]; then echo "  FAIL: pas de ligne BENCH (run $i)"; exit 1; fi
    printf '  run %d : %s µs/trame\n' "$i" "$us"
    if [ -z "$best" ] || awk "BEGIN{exit !($us < $best)}"; then best=$us; fi
done

pct=$(awk "BEGIN{printf \"%.1f\", $best / 200.0}")   # 20,000 µs = 100 %
if awk "BEGIN{exit !($best <= $BUDGET)}"; then
    echo "  PASS: ${best} µs/trame (${pct} % du budget de 20 ms, plafond ${BUDGET} µs)"
    exit 0
fi
if [ -n "$throttled" ]; then
    echo "  SKIP: ${best} µs/trame (${pct} %) hors budget, mais machine bridée : ${throttled}"
    echo "        → mesure non concluante ; BENCH_STRICT=1 pour trancher quand même"
    exit 0
fi
echo "  FAIL: ${best} µs/trame dépasse le plafond de ${BUDGET} µs (${pct} % de la trame)"
echo "        → profiler avant d'ajuster le plafond (docs/specs/V2_CYCLE_ACCURACY.md, US7.1)"
exit 1
