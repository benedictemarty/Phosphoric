#!/usr/bin/env bash
#
# test_serial_file_backend.sh — deterministic file: replay/capture transport
#
# The file: backend is a transparent byte pipe: bytes read from an input file
# are delivered to the Oric as RX; bytes the Oric transmits are appended to an
# output file. Being transparent it is shared by --serial and --dtl2000.
#
# Three checks, all hermetic (no network, no peer):
#   1. Capture     — dtl2000-test.bas transmits 'A'..'J'; assert the capture
#                    file holds exactly "ABCDEFGHIJ".
#   2. Round-trip  — dtl2000-echo.bas reads 5 replayed RX bytes and echoes them
#                    back on TX; assert input file == output file (proves BOTH
#                    replay and capture in one run).
#   3. Plumbing    — the same file: spec is accepted behind --serial too.
#
# Usage:  tests/integration/test_serial_file_backend.sh
# Exit:   0 = pass, 1 = fail
#
# Author: bmarty <bmarty@mailo.com>

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
BAS2TAP="./bas2tap"
TMP="$(mktemp -d /tmp/serial_file.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== serial file: backend (replay/capture) test ==="

# --- preconditions (skip gracefully) ---------------------------------------
[ -x "$EMU" ]     || { echo "  [SKIP] $EMU not built (make SDL2=1)"; exit 0; }
[ -f "$ROM" ]     || { echo "  [SKIP] $ROM absent"; exit 0; }
[ -x "$BAS2TAP" ] || { echo "  [SKIP] $BAS2TAP not built (make tools)"; exit 0; }

# --- 1) capture: DTL transmits A..J -> capture file ------------------------
"$BAS2TAP" examples/dtl2000-test.bas -o "$TMP/test.tap" --auto-run >/dev/null 2>&1
cap="$TMP/cap.bin"
: > "$cap"
"$EMU" -r "$ROM" --dtl2000 "file::$cap" -t "$TMP/test.tap" -f \
    -n -c 18000000 >/dev/null 2>&1
got="$(tr -d '\0' < "$cap")"
[ "$got" = "ABCDEFGHIJ" ] \
    && ok "capture file holds the 10 transmitted bytes (ABCDEFGHIJ)" \
    || ko "capture mismatch: got '$got' (expected ABCDEFGHIJ)"

# --- 2) round-trip: replay RX -> echo -> capture TX ------------------------
"$BAS2TAP" examples/dtl2000-echo.bas -o "$TMP/echo.tap" --auto-run >/dev/null 2>&1
inp="$TMP/in.bin"; out="$TMP/out.bin"
printf 'ORIC!' > "$inp"; : > "$out"
"$EMU" -r "$ROM" --dtl2000 "file:$inp:$out" -t "$TMP/echo.tap" -f \
    -n -c 10000000 >/dev/null 2>&1
if cmp -s "$inp" "$out"; then
    ok "round-trip replay->echo->capture: output == input (ORIC!)"
else
    ko "round-trip mismatch: $(xxd -p "$out" 2>/dev/null) != $(xxd -p "$inp")"
fi

# --- 3) plumbing: file: is accepted behind --serial as well ----------------
plog="$TMP/serial.log"
"$EMU" -r "$ROM" --serial "file::$TMP/s.bin" -n -c 200000 >"$plog" 2>&1
grep -q "Serial FILE:" "$plog" && grep -q "Serial interface enabled" "$plog" \
    && ok "--serial accepts the file: transport (shared helper)" \
    || ko "--serial did not accept the file: transport"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
