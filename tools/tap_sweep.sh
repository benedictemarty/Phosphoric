#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# tap_sweep.sh — run every .tap of a folder and flag the suspicious ones (2.31.8).
#
# Usage : tools/tap_sweep.sh DIR [JOBS] > results.txt
#
# Each tape is loaded the normal way (auto CLOAD"", no fast-load -f) on the
# Atmos ROM for 25 M cycles, then classified from the CPU state and the text
# screen ($BB80) at 24 M cycles:
#   HALT     the 6502 stopped (JAM opcode) before the end
#   TIMEOUT  the emulator did not finish within 30 s
#   ERROR    "ERROR" on the screen (BASIC error, TAPE ERROR...)
#   READY    the screen ends on CLOAD"" / Ready (often: no auto-run flag)
#   NOSCREEN no screen captured at 24 M cycles (the run ended early)
#   OK       none of the above — NOT a proof that the program is right
# HALT / ERROR / READY tapes are run again on the ORIC-1 ROM (BASIC 1.0):
# many tapes are ORIC-1 programs. The verdict line gives both results.
# Output: one line per tape, "verdict11|verdict10|file|detail|screen".
# TAP_SWEEP_SHOTS=DIR also saves the screen of each tape (Atmos run) as
# DIR/<name>.ppm, for tools/contact_sheet.py (2.31.10) — HIRES screens are
# invisible to the text checks above.
# Not part of `make tests` (1318 tapes ≈ 20 min with 4 jobs).
#
# Author: bmarty <bmarty@mailo.com>

set -u
DIR=${1:?usage: tools/tap_sweep.sh DIR [JOBS]}
JOBS=${2:-4}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

one() {
    local rom="$1" tap="$2" bin log rc halt pc scr st n
    bin=$(mktemp); log=$(mktemp)
    local shot=()
    if [ -n "${TAP_SWEEP_SHOTS:-}" ] && [ "$rom" = basic11b.rom ]; then
        shot=(--screenshot-at "24000000:$TAP_SWEEP_SHOTS/$(basename "$tap").ppm")
    fi
    timeout 30 "$ROOT/oric1-emu" --no-config -r "$ROOT/roms/$rom" -t "$tap" --headless \
        -c 25000000 --dump-ram-at "24000000:$bin" "${shot[@]}" >"$log" 2>&1; rc=$?   # logs on stdout
    halt=$(grep -o "CPU halted after [0-9]* cycles" "$log" | head -1)
    pc=$(grep -o "PC:[0-9A-F]*" "$log" | tail -1)
    scr=$(python3 -I -c "
import sys
d = open(sys.argv[1], 'rb').read()
s = d[0xBB80:0xBB80 + 40 * 28] if len(d) > 0xBFE0 else b''
rows = (''.join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else ' '
                for c in s[r * 40:(r + 1) * 40]).strip() for r in range(28))
print(' / '.join(x for x in rows if x))" "$bin" 2>/dev/null)
    local dumped=0
    [ -s "$bin" ] && dumped=1
    rm -f "$bin" "$log"
    st=OK
    if [ "$rc" -eq 124 ]; then st=TIMEOUT
    elif [ -n "$halt" ]; then
        n=${halt//[!0-9]/}
        [ "$n" -lt 24900000 ] && st=HALT
    fi
    [ "$st" = OK ] && [ "$dumped" = 0 ] && st=NOSCREEN   # no RAM dump at 24 M: run ended early
    [ "$st" = OK ] && echo "$scr" | grep -q "ERROR" && st=ERROR
    [ "$st" = OK ] && echo "$scr" | grep -qE 'CLOAD"" / Ready *$' && st=READY
    printf '%s\t%s\t%s\n' "$st" "$halt $pc" "${scr:0:200}"
}

sweep_one() {
    local tap="$1" r11 r10="-" v11 v10="-"
    r11=$(one basic11b.rom "$tap"); v11=${r11%%$'\t'*}
    case "$v11" in HALT|ERROR|READY|TIMEOUT|NOSCREEN)
        r10=$(one basic10.rom "$tap"); v10=${r10%%$'\t'*};;
    esac
    local res=$r11
    [ "$v10" = OK ] && res=$r10
    IFS=$'\t' read -r _ detail screen <<<"$res"
    echo "$v11|$v10|$(basename "$tap")|$detail|$screen"
}
export -f one sweep_one
export ROOT

[ -x "$ROOT/oric1-emu" ] || { echo "oric1-emu not built" >&2; exit 1; }
find "$DIR" -maxdepth 1 -type f -iname '*.tap' -print0 | sort -z |
    xargs -0 -P "$JOBS" -I{} bash -c 'sweep_one "$1"' _ {}
