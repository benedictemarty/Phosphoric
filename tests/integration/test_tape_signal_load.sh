#!/bin/sh
# test_tape_signal_load.sh — CLOAD au niveau SIGNAL, sur les deux ROM (V2-E6).
#
# Le mode `--tape-signal` fait lire la bande par la vraie routine ROM, à partir
# de la forme d'onde sur CB1 — c'est ce dont ont besoin les chargeurs maison et
# les protections. Ce chemin n'était couvert par AUCUN test : `test_tape_roundtrip`
# capture bien un CSAVE au signal, mais recharge ensuite en fast-load (`-f`).
#
# D'où ce test, qui vérifie les deux choses qui comptent, sur ORIC-1 ET Atmos :
#   1. le programme arrive réellement en mémoire ;
#   2. la ROM ne signale PAS d'erreur.
#
# C'est le point 2 qui manquait : la trame était encodée avec une parité PAIRE
# alors que le format ORIC est en parité IMPAIRE. La ROM 1.0 ne le voyait pas,
# la ROM 1.1 chargeait correctement puis affichait « Errors found ».

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

EMU=./oric1-emu
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { echo "  PASS: $1"; pass=$((pass + 1)); }
ko() { echo "  FAIL: $1"; fail=$((fail + 1)); }

[ -x "$EMU" ] || { echo "  SKIP: $EMU non construit"; exit 0; }
[ -x ./bas2tap ] || { echo "  SKIP: bas2tap non construit (make tools)"; exit 0; }

echo "=== CLOAD au niveau signal (--tape-signal) ==="

printf '10 PRINT"SIGNAL OK"\n20 END\n' > "$TMP/sig.bas"
./bas2tap "$TMP/sig.bas" -o "$TMP/sig.tap" >/dev/null 2>&1 \
    || { ko "bas2tap a échoué"; echo "Tests failed: $fail"; exit 1; }

for rom in basic10 basic11b; do
    [ -f "roms/$rom.rom" ] || { echo "  SKIP: roms/$rom.rom absent"; continue; }

    "$EMU" -r "roms/$rom.rom" -t "$TMP/sig.tap" --tape-signal -n -c 60000000 \
        --type-keys-when 'BC9A:52:CLOAD""\n' \
        --dump-ram-at "55000000:$TMP/$rom.bin" \
        --screenshot-text "$TMP/$rom.txt" >/dev/null 2>&1

    if [ -f "$TMP/$rom.bin" ] && grep -qa 'SIGNAL OK' "$TMP/$rom.bin"; then
        ok "$rom : le programme est chargé en mémoire par la vraie routine ROM"
    else
        ko "$rom : le programme n'est PAS arrivé en mémoire"
    fi

    if [ -f "$TMP/$rom.txt" ] && grep -q 'Errors found' "$TMP/$rom.txt"; then
        ko "$rom : la ROM signale « Errors found » (parité de trame ?)"
    else
        ok "$rom : chargement sans erreur signalée"
    fi
done

echo "---"
echo "Tests passed: $pass"
echo "Tests failed: $fail"
[ "$fail" -eq 0 ]
