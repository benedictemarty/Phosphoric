#!/usr/bin/env bash
# tools/bench_check.sh — budget de performance BLOQUANT (V2-E7, US7.1).
#
# `make bench` mesure ; ce script TRANCHE. Une trame PAL émulée dispose de
# 20 ms de temps réel ; la V2 s'engage à ce que l'émulation en consomme au
# plus 5 % (1000 µs) sur la machine de référence — de la marge pour le rendu,
# le son et les périphériques, et une alerte franche si un épic « au cycle »
# fait exploser le coût.
#
# Mesure : le scénario le plus léger (boot BASIC, headless), qui isole le
# cœur CPU + horloge maître + ULA + VIA + PSG sans I/O disque. Le chiffre
# suivi de sprint en sprint est ce frame_us-là (491 → 521 → 555 → 611 µs).
#
# Usage : tools/bench_check.sh                 (budget 1000 µs, 20 M cycles)
#         BENCH_BUDGET_US=1500 tools/bench_check.sh   (CI lente : relever)
#         BENCH_CYCLES=100000000 tools/bench_check.sh
# Sortie : 0 si sous budget, 1 sinon, 0 + SKIP si la ROM ou le binaire manquent.

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

# Une machine bridée (portable sur batterie, profil « low-power », fréquence
# effondrée) ne mesure pas l'émulateur mais sa propre économie d'énergie : le
# même binaire y coûte 1040 µs contre 601 µs à pleine vitesse. Dans ce cas on
# mesure et on AFFICHE, mais on ne tranche pas (SKIP, pas FAIL). BENCH_STRICT=1
# force le verdict quand même (CI, machine de référence).
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

# Le meilleur de RUNS mesures : on juge l'émulateur, pas la charge de la machine.
best=""
for i in $(seq 1 "$RUNS"); do
    out=$("$EMU" -r "$ROM" -n --bench -c "$CYCLES" 2>/dev/null | grep -F "BENCH " | head -n 1)
    us=$(sed -n 's/.*frame_us=\([0-9.]*\).*/\1/p' <<<"$out")
    if [ -z "$us" ]; then echo "  FAIL: pas de ligne BENCH (run $i)"; exit 1; fi
    printf '  run %d : %s µs/trame\n' "$i" "$us"
    if [ -z "$best" ] || awk "BEGIN{exit !($us < $best)}"; then best=$us; fi
done

pct=$(awk "BEGIN{printf \"%.1f\", $best / 200.0}")   # 20 000 µs = 100 %
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
