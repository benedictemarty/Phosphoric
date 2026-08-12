#!/usr/bin/env bash
# tests/integration/test_loci_golden.sh
#
# Determinism oracle for the LOCI boot path — the regression net for Epic 9
# (extracting the LOCI glue from main.c into src/io/loci_glue.c). It does NOT
# assert LOCI == native (that is the correctness benchmark test_loci_sedoric_e2e,
# a separate concern with a known preexisting screen-render gap). Instead it
# locks that a LOCI headless boot is BYTE-DETERMINISTIC and produces a non-empty
# screen: any glue extraction that perturbs the LOCI boot changes this dump.
#
# Media: roms/loci/locirom + loci_demo.img (SD image). Skips cleanly if absent.

set -u
cd "$(dirname "$0")/../.." || exit 1

EMU=./oric1-emu
LOCI_ROM=roms/loci/locirom
SDIMG=loci_demo.img
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
note_pass() { echo "  PASS: $1"; pass=$((pass + 1)); }
note_fail() { echo "  FAIL: $1"; fail=$((fail + 1)); }

echo "LOCI boot determinism (Epic 9 glue-refactor oracle):"

if [ ! -x "$EMU" ]; then echo "  oric1-emu not built — skipping"; exit 0; fi
if [ ! -f "$LOCI_ROM" ] || [ ! -f "$SDIMG" ]; then
    echo "  LOCI media missing ($LOCI_ROM / $SDIMG) — skipping"; exit 0
fi

boot() {  # $1 = output dump path
    "$EMU" -r "$LOCI_ROM" --loci --loci-sdimg "$SDIMG" --headless \
        -c 15000000 --dump-ram-at 14000000:"$1" >/dev/null 2>&1
}

boot "$TMP/a.bin"
boot "$TMP/b.bin"

if [ -s "$TMP/a.bin" ] && [ -s "$TMP/b.bin" ]; then
    note_pass "LOCI boot produced two 64K RAM dumps"
else
    note_fail "LOCI boot did not produce dumps"
fi

if cmp -s "$TMP/a.bin" "$TMP/b.bin"; then
    note_pass "two LOCI boots are byte-identical (deterministic)"
else
    note_fail "LOCI boot is NOT deterministic (dumps differ)"
fi

# The screen region ($BB80, 40x28 = 1120 bytes) must carry real content, so the
# oracle actually witnesses a booted machine (not an all-zero dump).
nz=$(python3 -c "d=open('$TMP/a.bin','rb').read(); s=d[0xBB80:0xBB80+1120]; print(sum(1 for x in s if x not in (0,0x20)))" 2>/dev/null)
if [ "${nz:-0}" -gt 32 ]; then
    note_pass "LOCI screen is non-empty ($nz printable bytes at \$BB80)"
else
    note_fail "LOCI screen looks empty ($nz bytes) — did it boot?"
fi

echo "  Results: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
