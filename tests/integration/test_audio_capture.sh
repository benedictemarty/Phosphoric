#!/usr/bin/env bash
# tests/integration/test_audio_capture.sh
#
# Regression for the headless audio-capture options --audio-wav / --psg-trace.
# These mirror --screenshot-at for sound : they let an automated headless run
# validate PSG output (measured, not inferred). Covered here :
#   1. --audio-wav without --headless is refused (SDL owns the PSG generator).
#   2. A known ROM sound (PING) yields a valid, NON-silent 44.1 kHz stereo WAV
#      and a --psg-trace that contains sound-register writes (R0..R13).
#   3. The capture is DETERMINISTIC (two runs -> byte-identical WAV).

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

echo "Audio capture (--audio-wav / --psg-trace) tests:"

# ── Test 1: --audio-wav without --headless → hard error, exit 1 ────────
out=$("$EMU" -r "$ROM" --audio-wav "$TMP/x.wav" -c 1000 2>&1)
rc=$?
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "audio-wav requires --headless"; then
    note_pass "--audio-wav without --headless exits 1 with clear error"
else
    note_fail "--audio-wav without --headless (rc=$rc, expected 1)"
    echo "$out" | tail -2 | sed 's/^/        /'
fi

# ── Test 2: headless PING → valid non-silent WAV + PSG trace ───────────
"$EMU" -r "$ROM" -n --type-keys '2500000:PING\n' \
    --audio-wav "$TMP/ping.wav" --psg-trace "$TMP/ping.psg" \
    -c 6000000 >/dev/null 2>&1

if python3 - "$TMP/ping.wav" <<'PY'
import sys, wave, array
w = wave.open(sys.argv[1], 'rb')
assert w.getnchannels() == 2 and w.getframerate() == 44100 and w.getsampwidth() == 2, "format"
a = array.array('h'); a.frombytes(w.readframes(w.getnframes()))
nz = sum(1 for s in a if s != 0)
assert nz > 1000, "WAV is silent (%d non-zero)" % nz
PY
then
    note_pass "PING → valid 44.1 kHz stereo WAV, non-silent"
else
    note_fail "PING WAV invalid or silent"
fi

if [ -s "$TMP/ping.psg" ] && grep -qE '^[0-9]+ R([0-9]|1[0-3])=' "$TMP/ping.psg"; then
    note_pass "--psg-trace logged sound-register writes (R0..R13)"
else
    note_fail "--psg-trace empty or missing register writes"
fi

# ── Test 3: capture is deterministic (byte-identical WAV) ──────────────
"$EMU" -r "$ROM" -n --type-keys '2500000:PING\n' \
    --audio-wav "$TMP/ping2.wav" -c 6000000 >/dev/null 2>&1
if cmp -s "$TMP/ping.wav" "$TMP/ping2.wav"; then
    note_pass "two runs → byte-identical WAV (deterministic)"
else
    note_fail "WAV differs between runs (non-deterministic)"
fi

echo ""
echo "  Results: $pass passed, $fail failed (total: $((pass + fail)))"
[ "$fail" -eq 0 ]
