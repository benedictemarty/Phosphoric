#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# dsk_sweep.sh — boot every .dsk of a folder twice and flag the suspicious ones (2.31.12).
#
# Usage: tools/dsk_sweep.sh DIR [JOBS] > results.txt
#
# Each disk boots on the Atmos ROM + Microdisc ROM for 30 M cycles, once with
# the real WD1793 timing (default: bytes paced by the platter since 2.31.9)
# and once with --fdc-timing fast (bytes served on CPU demand). Per run:
#   HALT     the 6502 stopped (JAM opcode) before the end
#   TIMEOUT  the emulator did not finish within 45 s
#   LOST     the FDC reported lost bytes (LOST DATA, S2) during the session
#   NOSCREEN no RAM dump at 29 M cycles (run ended early)
#   OK       none of the above — NOT a proof that the program is right
# A disk that is OK in fast but not in real points at the platter model.
# Output: "real|fast|file|detail|screen" (screen = text at $BB80, real run).
# DSK_SWEEP_SHOTS=DIR saves the real-timing screen as DIR/<name>.ppm
# (tools/contact_sheet.py builds contact sheets from them).
# The emulator logs on STDOUT: capture both streams.
#
# Author: bmarty <bmarty@mailo.com>

set -u
DIR=${1:?usage : tools/dsk_sweep.sh DOSSIER [TÂCHES]}
JOBS=${2:-4}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

run() {   # run TIMING DSK SHOT → "verdict<TAB>detail<TAB>screen"
    local timing="$1" dsk="$2" shot="$3" bin log rc halt pc lost scr st n dumped=0
    bin=$(mktemp); log=$(mktemp)
    local extra=()
    [ -n "$shot" ] && extra=(--screenshot-at "29000000:$shot")
    timeout 45 "$ROOT/oric1-emu" --no-config -r "$ROOT/roms/basic11b.rom" \
        --disk-rom "$ROOT/roms/microdis.rom" -d "$dsk" --fdc-timing "$timing" -n \
        -c 30000000 --dump-ram-at "29000000:$bin" "${extra[@]}" >"$log" 2>&1; rc=$?
    halt=$(grep -o "CPU halted after [0-9]* cycles" "$log" | head -1)
    pc=$(grep -o "PC:[0-9A-F]*" "$log" | tail -1)
    lost=$(grep -o "FDC: [0-9]* octet(s) signalé(s) perdus" "$log" | head -1)
    [ -s "$bin" ] && dumped=1
    scr=$(python3 -I -c "
import sys
d = open(sys.argv[1], 'rb').read()
s = d[0xBB80:0xBB80 + 40 * 28] if len(d) > 0xBFE0 else b''
rows = (''.join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else ' '
                for c in s[r * 40:(r + 1) * 40]).strip() for r in range(28))
print(' / '.join(x for x in rows if x))" "$bin" 2>/dev/null)
    rm -f "$bin" "$log"
    st=OK
    if [ "$rc" -eq 124 ]; then st=TIMEOUT
    elif [ -n "$halt" ]; then
        n=${halt//[!0-9]/}
        [ "$n" -lt 29900000 ] && st=HALT
    fi
    [ "$st" = OK ] && [ -n "$lost" ] && st=LOST
    [ "$st" = OK ] && [ "$dumped" = 0 ] && st=NOSCREEN
    printf '%s\t%s\t%s\n' "$st" "$halt $pc $lost" "${scr:0:200}"
}

sweep_one() {
    local dsk="$1" name shot="" r f vr vf
    name=$(basename "$dsk")
    [ -n "${DSK_SWEEP_SHOTS:-}" ] && shot="$DSK_SWEEP_SHOTS/$name.ppm"
    r=$(run real "$dsk" "$shot"); f=$(run fast "$dsk" "")
    vr=${r%%$'\t'*}; vf=${f%%$'\t'*}
    IFS=$'\t' read -r _ detail screen <<<"$r"
    echo "$vr|$vf|$name|$detail|$screen"
}
export -f run sweep_one
export ROOT

[ -x "$ROOT/oric1-emu" ] || { echo "oric1-emu non construit" >&2; exit 1; }
find "$DIR" -maxdepth 1 -type f -iname '*.dsk' -print0 | sort -z |
    xargs -0 -P "$JOBS" -I{} bash -c 'sweep_one "$1"' _ {}
