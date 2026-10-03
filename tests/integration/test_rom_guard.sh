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
#
# 2.20.0 — profil minimal : sans -r, la ROM système par défaut est chargée
# (roms/basic10.rom, ORIC-1 nu ; roms/basic11b.rom sous -m atmos). La garde
# ne s'applique plus qu'avec --no-rom (zone $C000-$FFFF volontairement vide).

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

# ── Test 1: --disk-rom with --no-rom → hard error, exit 1 ──────────
out=$("$EMU" -n --no-rom --disk-rom "$DISKROM" -m atmos -c 1000 2>&1)
rc=$?
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "disk-rom requires the base system ROM"; then
    note_pass "--disk-rom with --no-rom exits 1 with clear error"
else
    note_fail "--disk-rom with --no-rom (rc=$rc, expected 1)"
    echo "$out" | tail -2 | sed 's/^/        /'
fi

# ── Test 2: --no-rom, no --disk-rom → non-fatal warning, exit 0 ─────
out=$("$EMU" -n --no-rom -m atmos -c 1000 2>&1)
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

# ── Test 4: no -r → minimal profile, bare ORIC-1 on BASIC 1.0 ───────
shot=$(mktemp)
out=$("$EMU" -n -c 3000000 --screenshot-text "$shot" 2>&1)
rc=$?
if [ "$rc" -eq 0 ] \
   && echo "$out" | grep -q "Profil minimal : ORIC-1, ROM par défaut roms/basic10.rom" \
   && echo "$out" | grep -q "Machine model: ORIC-1" \
   && ! echo "$out" | grep -qE "No system ROM loaded|WARNING" \
   && grep -q "ORIC EXTENDED BASIC V1.0" "$shot"; then
    note_pass "no -r boots the minimal profile (ORIC-1, BASIC 1.0, no warning)"
else
    note_fail "minimal profile (rc=$rc)"
    echo "$out" | tail -3 | sed 's/^/        /'
fi

# ── Test 5: no -r but -m atmos → BASIC 1.1 ROM by default ───────────
out=$("$EMU" -n -m atmos -c 3000000 --screenshot-text "$shot" 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "ROM par défaut roms/basic11b.rom" \
   && grep -q "ORIC EXTENDED BASIC V1.1" "$shot"; then
    note_pass "-m atmos without -r boots BASIC 1.1"
else
    note_fail "-m atmos default ROM (rc=$rc)"
    echo "$out" | tail -3 | sed 's/^/        /'
fi

# ── Test 6: default ROM found next to the executable (other cwd) ─────
out=$(cd "$(mktemp -d)" && "$OLDPWD/$EMU" -n -c 1000 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "ROM par défaut /.*roms/basic10.rom"; then
    note_pass "default ROM resolved next to the executable"
else
    note_fail "default ROM from another cwd (rc=$rc)"
    echo "$out" | tail -3 | sed 's/^/        /'
fi
rm -f "$shot"

echo ""
echo "  Results: $pass passed, $fail failed (total: $((pass + fail)))"
[ "$fail" -eq 0 ]
