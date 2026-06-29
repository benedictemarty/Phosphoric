#!/usr/bin/env bash
#
# test_control_media_swap.sh — --control hot-swap commands (load/eject media)
#
# Drives the --control IPC protocol over a pipe and asserts the new media
# hot-swap commands: load-disk / eject-disk / eject-tape. Hermetic: one
# emulator process per scenario, fed a fixed command script on stdin.
#
# Checks:
#   1. hello advertises the new caps (load-disk, eject-disk, eject-tape).
#   2. load-disk <drive> <path> mounts a .dsk (OK drive=X) and peek disk shows it.
#   3. eject-disk empties the drive; ejecting an empty drive is an error.
#   4. bad drive selector and missing file are rejected with ERR.
#   5. eject-tape unloads a loaded tape; with no tape it is an error.
#
# Usage:  tests/integration/test_control_media_swap.sh
# Exit:   0 = pass (or graceful skip), 1 = fail
#
# Author: bmarty <bmarty@mailo.com>

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
DISKROM="roms/microdis.rom"
DSK="disks/3dfongus.dsk"
TAP="examples/graphics.tap"

pass=0
fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== --control media hot-swap test ==="

[ -x "$EMU" ]      || { echo "  [SKIP] $EMU not built"; exit 0; }
[ -f "$ROM" ]      || { echo "  [SKIP] $ROM absent"; exit 0; }
[ -f "$DISKROM" ]  || { echo "  [SKIP] $DISKROM absent"; exit 0; }
[ -f "$DSK" ]      || { echo "  [SKIP] $DSK absent"; exit 0; }
[ -f "$TAP" ]      || { echo "  [SKIP] $TAP absent"; exit 0; }

# Run a newline-separated command script through --control, echo the output.
run_control() {
    printf '%b' "$1" | timeout 10 "$EMU" -r "$ROM" --disk-rom "$DISKROM" --control 2>/dev/null
}

# --- scenario 1: caps + load/peek/eject disk ------------------------------
OUT="$(run_control 'hello\nload-disk B '"$DSK"'\npeek disk\neject-disk B\neject-disk B\nquit\n')"

echo "$OUT" | grep -q "caps=.*load-disk.*eject-disk.*eject-tape" \
    && ok "hello advertises the hot-swap caps" \
    || ko "hello missing hot-swap caps"

echo "$OUT" | grep -q "^OK drive=B size=[0-9]" \
    && ok "load-disk B mounts the .dsk (OK drive=B size=...)" \
    || ko "load-disk B did not report a mount"

echo "$OUT" | grep -q "drives_mounted=0100" \
    && ok "peek disk shows drive B mounted (drives_mounted=0100)" \
    || ko "peek disk does not show drive B mounted"

echo "$OUT" | grep -q "^OK drive=B ejected" \
    && ok "eject-disk B empties the drive" \
    || ko "eject-disk B did not report an eject"

echo "$OUT" | grep -q "^ERR eject-disk: drive B already empty" \
    && ok "ejecting an already-empty drive is rejected" \
    || ko "double eject-disk not rejected"

# --- scenario 2: invalid selector + missing file --------------------------
OUT="$(run_control 'load-disk Z '"$DSK"'\nload-disk A /no/such/file.dsk\nquit\n')"

echo "$OUT" | grep -q "^ERR load-disk: drive must be A-D" \
    && ok "invalid drive selector rejected" \
    || ko "invalid drive selector not rejected"

echo "$OUT" | grep -q "^ERR load-disk: cannot read" \
    && ok "missing .dsk file rejected" \
    || ko "missing .dsk file not rejected"

# --- scenario 3: eject-tape -----------------------------------------------
OUT="$(run_control 'load-tap '"$TAP"'\neject-tape\neject-tape\nquit\n')"

echo "$OUT" | grep -q "^OK ejected" \
    && ok "eject-tape unloads a loaded tape" \
    || ko "eject-tape did not unload the tape"

echo "$OUT" | grep -q "^ERR eject-tape: no tape loaded" \
    && ok "eject-tape with no tape is rejected" \
    || ko "eject-tape with no tape not rejected"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
