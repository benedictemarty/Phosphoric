#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# test_printer_lprint.sh — LPRINT de bout en bout avec les vraies ROM (2.31.4).
#
# Avant 2.31.4, le STROBE de l'imprimante était pris sur CA2 du VIA, qui est la
# ligne BC1 du PSG : le fichier ne recevait que des numéros de registre du PSG
# ($0E, $00-$0D) et LPRINT bloquait la machine en attendant un ACK sur CA1 qui
# ne venait jamais. Les ROM impriment par ORA ($0301) + impulsion basse sur PB4
# (Atmos $F5C1, ORIC-1 $F57B), puis attendent l'ACK sur CA1.
#
# Vérifie, pour chaque ROM présente : le fichier contient exactement
# "HELLO\r\n", la machine continue après LPRINT (PRINT 77 → 77), et une
# session sans impression laisse le fichier vide (pas d'octet parasite au
# démarrage).
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

echo "=== LPRINT end-to-end (STROBE PB4 + ACK CA1) ==="
[ -x "$EMU" ] || { echo "  [SKIP] $EMU not built"; exit 0; }

for rom in basic11b basic10; do
    R="roms/$rom.rom"
    [ -f "$R" ] || { echo "  [SKIP] $R absent"; continue; }
    "$EMU" -n --no-config -r "$R" -p "$TMP/$rom.txt" \
        --type-keys '4000000:LPRINT "HELLO"\nPRINT 77\n' -c 12000000 \
        --screenshot-text-at 11900000:"$TMP/$rom.scr" >/dev/null 2>&1
    got="$(xxd -p "$TMP/$rom.txt" 2>/dev/null | tr -d '\n')"
    [ "$got" = "48454c4c4f0d0a" ] \
        && ok "$rom: LPRINT \"HELLO\" → file = HELLO\\r\\n" \
        || ko "$rom: file = '${got:-<empty>}' (expected 48454c4c4f0d0a)"
    grep -qE '^ +77 *$' "$TMP/$rom.scr" \
        && ok "$rom: the machine goes on after LPRINT (ACK on CA1)" \
        || ko "$rom: PRINT 77 not executed after LPRINT (hung waiting for ACK?)"
done

R="roms/basic11b.rom"
if [ -f "$R" ]; then
    "$EMU" -n --no-config -r "$R" -p "$TMP/idle.txt" -c 8000000 >/dev/null 2>&1
    sz="$(stat -c %s "$TMP/idle.txt" 2>/dev/null || echo -1)"
    [ "$sz" = "0" ] \
        && ok "no LPRINT: empty file (no stray byte at reset)" \
        || ko "no LPRINT: file has $sz bytes (expected 0)"
fi

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
