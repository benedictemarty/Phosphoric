#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# dsk_sweep.sh — démarre deux fois chaque .dsk d'un dossier et signale les suspects (2.31.12).
#
# Usage : tools/dsk_sweep.sh DOSSIER [TÂCHES] > resultats.txt
#
# Chaque disquette démarre sur ROM Atmos + ROM Microdisc pendant 30 M cycles,
# une fois avec le timing réel du WD1793 (défaut : octets au rythme du plateau
# depuis 2.31.9) et une fois avec --fdc-timing fast (octets servis à la demande
# du CPU). Verdict par passage :
#   HALT     le 6502 s'est arrêté (opcode JAM) avant la fin
#   TIMEOUT  l'émulateur n'a pas fini en 45 s
#   LOST     le FDC a signalé des octets perdus (LOST DATA, S2) pendant la session
#   NOSCREEN pas de vidage mémoire à 29 M cycles (passage terminé plus tôt)
#   OK       rien de tout cela — ce n'est PAS une preuve que le programme est juste
# Une disquette OK en fast mais pas en réel désigne le modèle du plateau.
# Sortie : « réel|fast|fichier|détail|écran » (écran = texte en $BB80, passage réel).
# DSK_SWEEP_SHOTS=DOSSIER enregistre l'écran du passage réel en DOSSIER/<nom>.ppm
# (tools/contact_sheet.py en fait des planches contact).
# L'émulateur journalise sur la SORTIE STANDARD : capturer les deux flux.
#
# Author: bmarty <bmarty@mailo.com>

set -u
DIR=${1:?usage : tools/dsk_sweep.sh DOSSIER [TÂCHES]}
JOBS=${2:-4}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

run() {   # run TIMING DSK CAPTURE → « verdict<TAB>détail<TAB>écran »
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
