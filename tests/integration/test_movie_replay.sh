#!/usr/bin/env bash
#
# test_movie_replay.sh — deterministic input record/replay (TAS movie)
#
# The keyboard matrix is the only non-deterministic input; recording it per
# frame and replaying it must reproduce a session bit-exactly. The core
# guarantee for TAS / bug-repro / CI regression is that REPLAY IS
# DETERMINISTIC: replaying the same movie always yields the same output.
#
# Checks (hermetic — headless, no network):
#   1. Record    — a scripted session writes a well-formed movie (header +
#                  change-only matrix events).
#   2. Determinism — replaying that movie twice produces byte-identical
#                  final screenshots.
#   3. Robustness — a corrupt movie is rejected (emulator still runs).
#
# Usage:  tests/integration/test_movie_replay.sh
# Exit:   0 = pass, 1 = fail
#
# Author: bmarty <bmarty@mailo.com>

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
TMP="$(mktemp -d /tmp/movie_replay.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== input record/replay (movie) test ==="

[ -x "$EMU" ] || { echo "  [SKIP] $EMU not built"; exit 0; }
[ -f "$ROM" ] || { echo "  [SKIP] $ROM absent"; exit 0; }

mov="$TMP/session.phm"

# --- 1) record a scripted session ------------------------------------------
"$EMU" -r "$ROM" -n --type-keys "200:PRINT 6*7" --record "$mov" \
    -c 4000000 >/dev/null 2>&1
if head -1 "$mov" 2>/dev/null | grep -q '^PHOSPHORIC-MOVIE 1$' \
   && grep -q '^model 1$' "$mov" \
   && [ "$(grep -c '^F ' "$mov")" -gt 0 ]; then
    ok "record wrote a well-formed movie ($(grep -c '^F ' "$mov") events)"
else
    ko "movie file malformed or empty"
fi

# --- 2) replay determinism: two replays must be byte-identical -------------
"$EMU" -r "$ROM" -n --replay "$mov" -c 4000000 --screenshot "$TMP/r1.ppm" \
    >/dev/null 2>&1
"$EMU" -r "$ROM" -n --replay "$mov" -c 4000000 --screenshot "$TMP/r2.ppm" \
    >/dev/null 2>&1
if [ -s "$TMP/r1.ppm" ] && cmp -s "$TMP/r1.ppm" "$TMP/r2.ppm"; then
    ok "replay is deterministic (two replays byte-identical)"
else
    ko "replay diverged between runs"
fi

# --- 3) corrupt movie is rejected, emulator still runs ---------------------
echo "not a movie" > "$TMP/bad.phm"
"$EMU" -r "$ROM" -n --replay "$TMP/bad.phm" -c 500000 \
    --screenshot "$TMP/bad.ppm" >/dev/null 2>&1
if [ -s "$TMP/bad.ppm" ]; then
    ok "corrupt movie rejected gracefully (emulator still ran)"
else
    ko "emulator failed on corrupt movie"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
