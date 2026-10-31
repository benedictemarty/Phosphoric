#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
#
# test_printer_lprint.sh — end-to-end LPRINT with the real ROMs (2.31.4).
#
# Before 2.31.4, the printer STROBE was taken from the VIA's CA2, which is the
# PSG BC1 line: the file only got PSG register numbers ($0E, $00-$0D) and
# LPRINT hung the machine waiting for an ACK on CA1 that never came. The ROMs
# print through ORA ($0301) + a low pulse on PB4 (Atmos $F5C1, ORIC-1 $F57B),
# then wait for the ACK on CA1.
#
# Checks, for each ROM present: the file holds exactly "HELLO\r\n", the
# machine goes on after LPRINT (PRINT 77 → 77), and a session without
# printing leaves the file empty (no stray byte at start-up).
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
