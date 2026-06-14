#!/usr/bin/env bash
# tests/integration/test_rom_guard.sh — sprint 57
#
# Regression: starting without a base system ROM (-r) must not silently boot
# into a zeroed $C000-$FFFF ROM area.
#
# Bug history: `oric1-emu --disk-rom microdis.rom -d demo.dsk -m atmos` (no -r)
# left the BASIC ROM area empty. A disc demo (Cybernova/Nova2026) that maps the
# BASIC ROM back in ($0314=$06) then JMPs into it read $00 = BRK and fell into
# the $0000 BRK loop around cycle ~349k — a crash that looked like a Microdisc
# overlay/banking bug but was just a missing -r. The guard now fails fast on
# --disk-rom without -r (impossible on real hardware: the BASIC ROM is soldered,
# the Microdisc EPROM is additional) and warns when no ROM is given at all.

set -u
cd "$(dirname "$0")/../.." || exit 1

EMU=./oric1-emu
ROM=roms/basic11b.rom
DISKROM=roms/microdis.rom

pass=0
fail=0

note_pass() { echo "  PASS: $1"; pass=$((pass + 1)); }
note_fail() { echo "  FAIL: $1"; fail=$((fail + 1)); }

if [ ! -x "$EMU" ]; then
    echo "  oric1-emu not built — skipping (run: make)"
    exit 0
fi
if [ ! -f "$DISKROM" ] || [ ! -f "$ROM" ]; then
    echo "  roms missing ($ROM / $DISKROM) — skipping"
    exit 0
fi

echo "ROM presence guard tests:"

# ── Test 1: --disk-rom without -r → hard error, exit 1 ──────────────
out=$("$EMU" -n --disk-rom "$DISKROM" -m atmos -c 1000 2>&1)
rc=$?
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "disk-rom requires the base system ROM"; then
    note_pass "--disk-rom without -r exits 1 with clear error"
else
    note_fail "--disk-rom without -r (rc=$rc, expected 1)"
    echo "$out" | tail -2 | sed 's/^/        /'
fi

# ── Test 2: no -r, no --disk-rom → non-fatal warning, exit 0 ────────
out=$("$EMU" -n -m atmos -c 1000 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "No system ROM loaded"; then
    note_pass "no ROM at all warns (non-fatal)"
else
    note_fail "no ROM warning (rc=$rc, expected 0)"
    echo "$out" | tail -2 | sed 's/^/        /'
fi

# ── Test 3: with -r → no guard error, boots ────────────────────────
out=$("$EMU" -r "$ROM" -n --disk-rom "$DISKROM" -m atmos -c 1000 2>&1)
rc=$?
if [ "$rc" -eq 0 ] \
   && ! echo "$out" | grep -q "disk-rom requires" \
   && ! echo "$out" | grep -q "No system ROM loaded"; then
    note_pass "with -r the guard stays silent and the machine boots"
else
    note_fail "with -r should be clean (rc=$rc)"
    echo "$out" | tail -2 | sed 's/^/        /'
fi

echo ""
echo "  Results: $pass passed, $fail failed (total: $((pass + fail)))"
[ "$fail" -eq 0 ]
