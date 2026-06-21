#!/usr/bin/env bash
#
# test_dtl2000_txrx.sh — Digitelec DTL 2000 TX/RX loopback trace test
#
# Boots the BASIC driver examples/dtl2000-test.bas on the faithful DTL 2000
# card (PIA 6821 + ACIA 6850 at $03F8) wired to the loopback backend, captures
# the new --serial-trace byte log, and asserts that every transmitted byte
# comes back on the receive side with an identical value.
#
# The trace itself is the deliverable: each TX line must be followed by an RX
# line with the same hex, proving the full ACIA 6850 transmit -> line ->
# receive path works end to end.
#
# Usage:  tests/test_dtl2000_txrx.sh
# Exit:   0 = pass, 1 = fail
#
# Author: bmarty <bmarty@mailo.com>

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
BAS="examples/dtl2000-test.bas"
TAP="examples/dtl2000-test.tap"
BAS2TAP="./bas2tap"
TRACE="$(mktemp /tmp/dtl2000_txrx.XXXXXX.log)"
CYCLES=18000000            # asym V23 TX is 75 baud -> ~1.3M cycles per 10 bytes
EXPECTED=10               # driver sends 'A'..'J'

pass=0
fail=0
note() { printf '  %s\n' "$*"; }
ok()   { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko()   { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== DTL 2000 TX/RX loopback trace test ==="

# --- preconditions (skip gracefully so `make tests` never fails on assets) --
[ -x "$EMU" ]     || { echo "  [SKIP] $EMU not built (make SDL2=1)"; exit 0; }
[ -f "$ROM" ]     || { echo "  [SKIP] $ROM absent"; exit 0; }
[ -x "$BAS2TAP" ] || { echo "  [SKIP] $BAS2TAP not built (make tools)"; exit 0; }

# --- (re)build the tape from source ---------------------------------------
"$BAS2TAP" "$BAS" -o "$TAP" --auto-run >/dev/null 2>&1 \
    && ok "tape rebuilt from $BAS" \
    || { ko "bas2tap failed"; exit 1; }

# --- run the emulator with the DTL loopback + serial trace -----------------
"$EMU" -r "$ROM" --dtl2000 loopback -t "$TAP" -f -n -c "$CYCLES" \
    --serial-trace "$TRACE" >/dev/null 2>&1

[ -s "$TRACE" ] && ok "serial trace produced ($(wc -l < "$TRACE") lines)" \
                || { ko "no serial trace generated"; exit 1; }

# --- assertions on the trace ----------------------------------------------
tx=$(grep -c '^TX' "$TRACE")
rx=$(grep -c '^RX' "$TRACE")
note "transmitted: $tx   received: $rx"

[ "$tx" -eq "$EXPECTED" ] && ok "TX count = $EXPECTED" \
                          || ko "TX count = $tx (expected $EXPECTED)"
[ "$rx" -eq "$EXPECTED" ] && ok "RX count = $EXPECTED" \
                          || ko "RX count = $rx (expected $EXPECTED)"

# Each TX hex must be echoed by the next RX hex (loopback fidelity).
mismatch=0
paired=$(grep -E '^(TX|RX)' "$TRACE" | awk '
    $1=="TX" { txhex=$2; expect=1; next }
    $1=="RX" { if (expect && $2!=txhex) bad++; expect=0; matched++ }
    END { print matched" "bad+0 }')
matched=${paired% *}; mismatch=${paired#* }
[ "$mismatch" -eq 0 ] && ok "every TX byte echoed identically in RX ($matched pairs)" \
                      || ko "$mismatch TX/RX byte mismatches"

# All bytes must report "sent" (line connected + carrier), none dropped.
dropped=$(grep -c 'dropped' "$TRACE")
[ "$dropped" -eq 0 ] && ok "no transmit dropped (line connected, carrier on)" \
                     || ko "$dropped transmit(s) dropped"

echo "--- trace excerpt ---"
sed -n '1,6p' "$TRACE" | sed 's/^/  /'

# --- unified transports (recommendation #1): the DTL shares the transparent ---
# byte-pipe transports with --serial (loopback/tcp/pty/com), but NOT the
# protocol-injecting backends. The DTL 2000 is dialled by its PIA, not Hayes AT,
# so 'modem' must be REJECTED behind the card.
mlog="$(mktemp /tmp/dtl2000_xport.XXXXXX.log)"

"$EMU" -r "$ROM" --dtl2000 pty -n -c 200000 >"$mlog" 2>&1
grep -q "DTL 2000 enabled.*transport: pty" "$mlog" \
    && ok "DTL accepts a transparent transport (pty)" \
    || ko "DTL rejected the 'pty' transport"

"$EMU" -r "$ROM" --dtl2000 modem -n -c 200000 >"$mlog" 2>&1
grep -q "Unknown DTL 2000 transport" "$mlog" \
    && ok "DTL rejects the Hayes 'modem' backend (dialled via PIA, not AT)" \
    || ko "DTL wrongly accepted the Hayes 'modem' backend"

"$EMU" -r "$ROM" --dtl2000 bogus -n -c 100000 >"$mlog" 2>&1
grep -q "Unknown DTL 2000 transport" "$mlog" \
    && ok "DTL rejects an unknown transport with a clear error" \
    || ko "DTL did not report the unknown transport"
rm -f "$mlog"

echo "=== result: $pass passed, $fail failed ==="
rm -f "$TRACE"
[ "$fail" -eq 0 ]
