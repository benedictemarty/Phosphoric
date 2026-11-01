#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# tap_sweep.sh — charge chaque .tap d'un dossier et signale les suspects (2.31.8).
#
# Usage : tools/tap_sweep.sh DOSSIER [TÂCHES] > resultats.txt
#
# Chaque cassette est chargée par le chemin normal (CLOAD"" automatique, sans
# fast-load -f) sur la ROM Atmos pendant 25 M cycles, puis classée d'après l'état
# du CPU et l'écran texte ($BB80) à 24 M cycles :
#   HALT     le 6502 s'est arrêté (opcode JAM) avant la fin
#   TIMEOUT  l'émulateur n'a pas fini en 30 s
#   ERROR    « ERROR » à l'écran (erreur BASIC, TAPE ERROR...)
#   READY    l'écran finit sur CLOAD"" / Ready (souvent : pas d'auto-run)
#   NOSCREEN pas de vidage mémoire à 24 M cycles (passage terminé plus tôt)
#   OK       rien de tout cela — ce n'est PAS une preuve que le programme est juste
# Les cassettes HALT / ERROR / READY / TIMEOUT / NOSCREEN sont relancées sur la
# ROM ORIC-1 (BASIC 1.0) : beaucoup sont des programmes ORIC-1. La ligne donne
# les deux verdicts : « verdict11|verdict10|fichier|détail|écran ».
# TAP_SWEEP_SHOTS=DOSSIER enregistre aussi l'écran de chaque cassette (passage
# Atmos) en DOSSIER/<nom>.ppm, pour tools/contact_sheet.py (2.31.10) — les écrans
# HIRES échappent aux vérifications texte ci-dessus.
# L'émulateur journalise sur la SORTIE STANDARD (2.31.10 : avant, l'outil lisait
# stderr et ne voyait aucun arrêt).
# Hors `make tests` (1318 cassettes ≈ 20 min avec 4 tâches).
#
# Author: bmarty <bmarty@mailo.com>

set -u
DIR=${1:?usage : tools/tap_sweep.sh DOSSIER [TÂCHES]}
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
        -c 25000000 --dump-ram-at "24000000:$bin" "${shot[@]}" >"$log" 2>&1; rc=$?   # journaux sur stdout
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
    [ "$st" = OK ] && [ "$dumped" = 0 ] && st=NOSCREEN   # pas de vidage à 24 M : passage terminé plus tôt
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

[ -x "$ROOT/oric1-emu" ] || { echo "oric1-emu non construit" >&2; exit 1; }
find "$DIR" -maxdepth 1 -type f -iname '*.tap' -print0 | sort -z |
    xargs -0 -P "$JOBS" -I{} bash -c 'sweep_one "$1"' _ {}
