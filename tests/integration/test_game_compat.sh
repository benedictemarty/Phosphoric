#!/usr/bin/env bash
# tests/integration/test_game_compat.sh — sprint 36b
#
# Boot/load a curated set of commercial Oric titles from
# OricProgramsLib and assert each one reaches a distinctive intro
# screen within N emulated cycles. Catches regressions in CPU/PSG/HIRES/
# tape pipeline that the synthetic unit tests can't see.
#
# OricProgramsLib is NOT versioned in the Phosphoric repo — set
# `ORIC_PROGRAMS_LIB=/path/to/lib` to override the default `$HOME/OricProgramsLib`.
# Missing files are skipped gracefully (so CI without the lib still passes).

set -u
cd "$(dirname "$0")/../.." || exit 1

EMU=./oric1-emu
BASIC_ROM=${BASIC_ROM:-roms/basic11b.rom}
LIB=${ORIC_PROGRAMS_LIB:-$HOME/OricProgramsLib}

pass=0
fail=0
skipped=0

if [ ! -x "$EMU" ]; then
    echo "ERROR: $EMU not built" >&2
    exit 1
fi
if [ ! -d "$LIB" ]; then
    echo "SKIP all (ORIC_PROGRAMS_LIB not found at $LIB)"
    exit 0
fi

# ── check : <slug> <tap_relpath> <cycles> <expected_text>
#
# Each entry runs the .tap in auto-CLOAD mode (no -f) for N emulated
# cycles, then dumps RAM and asserts the expected text is present in
# the 40x28 screen RAM ($BB80-$BFE0).
check_game() {
    local slug="$1" relpath="$2" cycles="$3" expected="$4"
    local tap="$LIB/Programs/$slug/files/$relpath"
    if [ ! -f "$tap" ]; then
        echo "  SKIP  ${slug}  (missing: $relpath)"
        skipped=$((skipped+1))
        return
    fi
    local bin
    bin=$(mktemp)
    local dump_at=$((cycles - 1000000))
    timeout 25 "$EMU" -r "$BASIC_ROM" -t "$tap" --headless \
        -c "$cycles" --dump-ram-at "${dump_at}:${bin}" >/dev/null 2>&1
    if [ ! -s "$bin" ]; then
        echo "  FAIL  ${slug}  (no RAM dump)"
        fail=$((fail+1))
        rm -f "$bin"
        return
    fi
    # Decode screen RAM (40x28 chars at $BB80) and grep expected text.
    local found
    found=$(python3 -c "
import sys
d = open('$bin','rb').read()
s = d[0xBB80:0xBB80+40*28]
text = ''
for r in range(28):
    line = s[r*40:(r+1)*40]
    text += ''.join(chr(b & 0x7F) if 32 <= (b & 0x7F) < 127 else ' ' for b in line) + '\n'
sys.stdout.write(text)" | grep -cF "$expected")
    rm -f "$bin"
    if [ "$found" -gt 0 ]; then
        echo "  PASS  ${slug}  (\"${expected}\")"
        pass=$((pass+1))
    else
        echo "  FAIL  ${slug}  (expected \"${expected}\" not found)"
        fail=$((fail+1))
    fi
}

echo "═══════════════════════════════════════════════════════════════"
echo "  Phosphoric game compatibility regression (sprint 36b)"
echo "  Lib : $LIB"
echo "═══════════════════════════════════════════════════════════════"

# Curated list. Each title was verified once by hand to identify a
# distinctive substring of its intro screen. Cycle budget tuned so a
# typical slow tape load fits.
check_game manic-miner    "MANICMINER.TAP"                    30000000 "PRESSEZ 3 FOIS RETURN"
check_game hopper         "hopper-o.tap"                      25000000 "PRESS  [1]  FOR NO SOUND"
check_game breakout       "BREAKOUT.TAP"                      25000000 "BALLS:"
check_game harrier-attack "harriera.tap"                      25000000 "HIGH  SCORE"
check_game atlantis       "Atlantis (F) (1985) [a1].tap"      25000000 "ARRETEZ VOTRE MAGNETO"
check_game oric-trek      "Oric Trek.tap"                     25000000 "Enter Level of Difficulty"
check_game tyrann         "Tyrann (F) (1984).tap"             25000000 "SI VOTRE ENREGISTREUR NE POSSEDE PAS"

echo "═══════════════════════════════════════════════════════════════"
echo "  Results: $pass passed, $fail failed, $skipped skipped"
echo "═══════════════════════════════════════════════════════════════"

if [ $fail -gt 0 ]; then
    exit 1
fi
exit 0
