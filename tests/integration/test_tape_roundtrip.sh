#!/usr/bin/env bash
# tests/integration/test_tape_roundtrip.sh
#
# Regression for the CSAVE tape-OUT capture / CLOAD roundtrip (voie A).
# The ROM CSAVE bit-bangs PB7 (Timer 1) as TWO half-pulses per bit (low 208,
# high 208/416). --tape-out-capture decodes that waveform back to a .TAP by
# timing RISING edge to RISING edge (full bit period 416 '1' / 624 '0') and
# hunting the start bit like ROM GetTapeByte ($E6C9). This locks two facts a
# past regression got wrong:
#   1. Rising-edge timing (NOT any-edge, which would measure 208/416 half-pulses
#      and read every bit as '1' -> empty capture).
#   2. Start-bit hunting framing (NOT a fixed 14-bit frame, which drifts one bit
#      per frame on a variable stop count -> doubled/garbled bytes).
# End-to-end proof: a program saved by CSAVE, captured, then CLOADed back, LISTs
# identically.

set -u
cd "$(dirname "$0")/../.." || exit 1

EMU=./oric1-emu
ROM=roms/basic11b.rom
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
note_pass() { echo "  PASS: $1"; pass=$((pass + 1)); }
note_fail() { echo "  FAIL: $1"; fail=$((fail + 1)); }

if [ ! -x "$EMU" ]; then
    echo "  oric1-emu not built — skipping (run: make)"
    exit 0
fi
if [ ! -f "$ROM" ]; then
    echo "  ROM missing ($ROM) — skipping"
    exit 0
fi

echo "Tape-OUT capture / CLOAD roundtrip (voie A) tests:"

# ── Step 1: type a program and CSAVE it, capturing the PB7 waveform ────────
# A deliberately NON-TRIVIAL program (string, tokens, multi-digit numbers, DATA)
# so the roundtrip exercises many non-zero bytes: a 1-bit framing drift is
# invisible on 0x00 bytes (x2 of 0 is 0) but corrupts the first non-zero byte,
# which is exactly the failure mode a leaderless/mis-phased decoder shows.
"$EMU" -r "$ROM" -n \
    --type-keys-when 'BC9A:52:10 PRINT"ABCDEFG":X=12345:GOTO 10\n20 DATA 99,88,77\nCSAVE"RT"\n' \
    --tape-out-capture "$TMP/rt.tap" -c 25000000 >/dev/null 2>&1

if [ ! -s "$TMP/rt.tap" ]; then
    note_fail "capture produced no file"
else
    size=$(wc -c < "$TMP/rt.tap")
    # A real capture is hundreds of bytes (sync leader + header + program);
    # the any-edge regression collapsed it to a single 0xFF byte.
    if [ "$size" -gt 32 ]; then
        note_pass "capture is non-trivial ($size bytes, not the 1-byte regression)"
    else
        note_fail "capture too small ($size bytes) — decoder likely broken"
    fi
    # The pilot leader must decode to a clean run of sync bytes (0x16).
    if od -An -tx1 "$TMP/rt.tap" | tr -s ' ' '\n' | grep -qc '^16$' \
       && [ "$(od -An -tx1 "$TMP/rt.tap" | tr -s ' ' '\n' | grep -c '^16$')" -gt 8 ]; then
        note_pass "decoded a clean 0x16 sync leader"
    else
        note_fail "no clean 0x16 sync leader (framing drift?)"
    fi
fi

# ── Step 2: CLOAD the captured .tap and LIST — the program must round-trip ─
"$EMU" -r "$ROM" -n -t "$TMP/rt.tap" -f \
    --type-keys-when 'BC9A:52:CLOAD"RT"\nLIST\n' \
    --screenshot-text "$TMP/list.txt" -c 18000000 >/dev/null 2>&1

# Both lines must come back byte-for-byte — a framing drift mangles the tokens.
if [ -f "$TMP/list.txt" ] \
   && grep -qE '10 PRINT"ABCDEFG":X=12345:GOTO 10' "$TMP/list.txt" \
   && grep -qE '20 DATA 99,88,77' "$TMP/list.txt"; then
    note_pass "CSAVE->capture->CLOAD roundtrip: non-zero-byte program listed back identically"
else
    note_fail "roundtrip: program did not LIST back identically (framing drift?)"
    [ -f "$TMP/list.txt" ] && sed -n '5,14p' "$TMP/list.txt"
fi

echo "  Results: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
