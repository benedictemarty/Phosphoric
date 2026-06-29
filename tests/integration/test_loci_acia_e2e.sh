#!/usr/bin/env bash
#
# test_loci_acia_e2e.sh — BASIC drives the LOCI ACIA 6551 at $0380
#
# End-to-end proof that a BASIC program running inside the emulator can both
# write to and read from the LOCI's ACIA 6551 at $0380 (the address the ACIA
# lands at under --loci, where the PicoWiFi modem is exposed). It exercises the
# exact path a real terminal program uses: poll TDRE, POKE the data register,
# poll RDRF, PEEK the data register.
#
# examples/loci_acia_e2e.bas (#4000 = work area, #3FFC = done sentinel):
#   - inits the 6551 (status reset, control 9600 8N1, command DTR/RTS on),
#   - for each byte of an "ATDT5551234"<CR> dial string: waits TDRE, transmits
#     it, waits RDRF, reads the echoed byte back, stores it at #4000+,
#   - posts a $FF sentinel and ends.
#
# The transport is the loopback backend: every transmitted byte is delivered
# straight back as RX. This makes the round-trip fully deterministic and
# hermetic (no network, no NVRAM, no host probing) while still proving the
# whole BASIC -> ACIA $0380 -> BASIC chain works under --loci.
#
# (The PicoWiFi modem backend answers real AT commands at this same $0380
# window; see examples/picowifi_test.bas for an interactive demo. It is not
# used here because its headless boot path is not yet deterministic enough for
# CI — tracked separately.)
#
# Two checks:
#   1. completion — BASIC reached its $FF done sentinel.
#   2. round-trip — the bytes BASIC read back equal the dial string it sent,
#                   proving TX (POKE $0380) and RX (PEEK $0380) both work.
#
# Usage:  tests/integration/test_loci_acia_e2e.sh
# Exit:   0 = pass (or graceful skip), 1 = fail
#
# Author: bmarty <bmarty@mailo.com>

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
BAS2TAP="./bas2tap"
BAS="examples/loci_acia_e2e.bas"
EXPECT="ATDT5551234"
TMP="$(mktemp -d /tmp/loci_acia.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== LOCI ACIA \$0380 BASIC e2e test ==="

# --- preconditions (skip gracefully) ---------------------------------------
[ -x "$EMU" ]     || { echo "  [SKIP] $EMU not built (make SDL2=1)"; exit 0; }
[ -f "$ROM" ]     || { echo "  [SKIP] $ROM absent"; exit 0; }
[ -x "$BAS2TAP" ] || { echo "  [SKIP] $BAS2TAP not built (make tools)"; exit 0; }
[ -f "$BAS" ]     || { echo "  [SKIP] $BAS absent"; exit 0; }

# --- compile + run ---------------------------------------------------------
"$BAS2TAP" "$BAS" -o "$TMP/t.tap" --auto-run >/dev/null 2>&1
ram="$TMP/ram.bin"
"$EMU" -r "$ROM" --loci --serial loopback \
    -t "$TMP/t.tap" -f -n -c 95000000 --dump-ram-at 90000000:"$ram" >/dev/null 2>&1

# --- 1) the BASIC program ran to completion --------------------------------
sentinel="$(dd if="$ram" bs=1 skip=$((0x3FFC)) count=1 2>/dev/null | od -An -tu1 | tr -d ' ')"
[ "$sentinel" = "255" ] \
    && ok "BASIC reached the \$FF done sentinel at #3FFC" \
    || ko "BASIC did not finish (sentinel=$sentinel, expected 255)"

# --- 2) round-trip: the read-back bytes equal the transmitted string -------
got="$(dd if="$ram" bs=1 skip=$((0x4000)) count="${#EXPECT}" 2>/dev/null | tr -d '\0')"
[ "$got" = "$EXPECT" ] \
    && ok "round-trip through ACIA \$0380 read back the sent string ($EXPECT)" \
    || ko "round-trip mismatch: read '$got' (expected '$EXPECT')"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
