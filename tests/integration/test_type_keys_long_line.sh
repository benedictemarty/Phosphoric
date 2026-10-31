#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# test_type_keys_long_line.sh — --type-keys après une ligne BASIC longue (2.31.5).
#
# Une ligne de plus de 40 colonnes, puis RETURN, puis « CLS:RUN » : pendant que
# le BASIC range la longue ligne, le scanner clavier (sous IRQ) continue de
# déposer les touches dans le tampon d'UNE touche de la ROM ($02DF) ; la touche
# suivante écrasait la précédente avant sa lecture, et « CLS:RUN » devenait
# « LS:RUN » (?SYNTAX ERROR). Rapport de la session du film. Depuis 2.31.5,
# l'auto-typer attend que la ROM ait lu la touche avant d'en presser une autre.
#
# Vérifie, pour chaque ROM présente : pas de ?SYNTAX ERROR, et le programme
# tourne (« COUNT » affiché). Avec une vraie fin de ligne et avec la séquence \n.
#
# Author: bmarty <bmarty@mailo.com>

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
EMU="./oric1-emu"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== --type-keys after a long BASIC line ==="
[ -x "$EMU" ] || { echo "  [SKIP] $EMU not built"; exit 0; }

LINE='10 I=I+1:PLOT 14,12,"COUNT "+STR$(I):GOTO 10'
for rom in basic11b basic10; do
    R="roms/$rom.rom"
    [ -f "$R" ] || { echo "  [SKIP] $R absent"; continue; }
    for form in raw esc; do
        if [ "$form" = raw ]; then
            KEYS="4000000:$LINE"$'\n'"CLS:RUN"$'\n'
        else
            KEYS="4000000:$LINE"'\nCLS:RUN\n'
        fi
        "$EMU" -n --no-config -r "$R" --type-keys "$KEYS" -c 12000000 \
            --screenshot-text-at 11900000:"$TMP/s.txt" >/dev/null 2>&1
        if grep -q "SYNTAX ERROR" "$TMP/s.txt"; then
            ko "$rom ($form): ?SYNTAX ERROR — a key after the long line was lost"
        elif grep -q "COUNT" "$TMP/s.txt"; then
            ok "$rom ($form): CLS:RUN typed whole, the program runs"
        else
            ko "$rom ($form): program not running (no COUNT on screen)"
        fi
    done
done

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
