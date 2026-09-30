#!/bin/sh
# test_tape_signal_load.sh -- CLOAD at SIGNAL level, on both ROMs (V2-E6).
#
# The `--tape-signal` mode makes the real ROM routine read the tape from the
# waveform on CB1 -- this is what custom loaders and copy protections need.
# This path was covered by NO test: `test_tape_roundtrip` does capture a CSAVE
# at signal level, but then reloads it with fast-load (`-f`).
#
# Hence this test, which checks the two things that matter, on ORIC-1 AND Atmos:
#   1. the program actually reaches memory;
#   2. the ROM reports NO error.
#
# Point 2 was the missing one: the frame was encoded with EVEN parity whereas
# the ORIC format uses ODD parity. ROM 1.0 did not notice, ROM 1.1 loaded
# correctly and then displayed "Errors found".

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
