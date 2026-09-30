#!/bin/sh
# test_raster_split.sh -- proof that the ULA fetches per cycle (V2-E4 / US4.4).
#
# A small machine-code program rewrites screen memory in a loop, alternating two
# ATTRIBUTE bytes: $10 (PAPER black) and $16 (PAPER cyan). The screen is thus
# constantly being rewritten while the beam scans it.
#
#   --ula-line  (pre-V2 behaviour): each scanline is sampled at a single
#               instant, the one at which it ends.
#   default     (one cell fetched per cycle): each cell sees memory at the
#               exact instant of its own fetch.
#
# Criterion: there are lines that differ between the two renders only OVER
# PART of their width. A partial difference proves that sampling is
# intra-line -- a cut in the middle of a line, which line-by-line rendering
# structurally cannot produce. This is the "raster split" effect exploited
# by Oric demos.
#
# The deterministic, exact case lives in `make test-clock`
# (test_ula_per_cycle_mid_line_split); this test is the proof on real 6502
# code executed by the full emulator.

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

EMU=./oric1-emu
ROM=roms/basic11b.rom
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { echo "  PASS: $1"; pass=$((pass + 1)); }
ko() { echo "  FAIL: $1"; fail=$((fail + 1)); }
finish() {
    echo "---"
    echo "Tests passed: $pass"
    echo "Tests failed: $fail"
    [ "$fail" -eq 0 ]
    exit $?
}

[ -x "$EMU" ] || { echo "  SKIP: $EMU non construit"; exit 0; }
[ -f "$ROM" ] || { echo "  SKIP: $ROM absent"; exit 0; }
[ -x ./bin2tap ] || { echo "  SKIP: bin2tap non construit (make tools)"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  SKIP: python3 requis pour l'analyse PPM"; exit 0; }

# ── The program, hand-assembled (32 bytes at $0500) ──
#   0500  A9 16        LDA #$16          ; PAPER cyan attribute
#   0502  A2 00        LDX #$00          ; <- loop
#   0504  9D 80 BB     STA $BB80,X
#   0507  9D 80 BC     STA $BC80,X
#   050A  9D 80 BD     STA $BD80,X
#   050D  9D 80 BE     STA $BE80,X
#   0510  E8           INX
#   0511  D0 F1        BNE $0504
#   0513  A2 60        LDX #$60
#   0515  CA           DEX               ; <- tail $BF80-$BFDF
#   0516  9D 80 BF     STA $BF80,X
#   0519  D0 FA        BNE $0515
#   051B  49 06        EOR #$06          ; toggle $16 <-> $10
#   051D  4C 02 05     JMP $0502
printf '\251\026\242\000\235\200\273\235\200\274\235\200\275\235\200\276\350\320\361\242\140\312\235\200\277\320\372\111\006\114\002\005' \
    >"$TMP/split.bin"
[ "$(wc -c <"$TMP/split.bin")" = "32" ] || { ko "binaire de test mal formé"; finish; }

./bin2tap "$TMP/split.bin" --start 0x0500 --exec 0x0500 -o "$TMP/split.tap" \
    --name SPLIT >/dev/null 2>&1 || { ko "bin2tap a échoué"; finish; }

run() { # $1 = options, $2 = PPM file
    "$EMU" -r "$ROM" -t "$TMP/split.tap" -f -n -c 12000000 $1 \
        --screenshot "$2" >/dev/null 2>&1
}

echo "=== Fetch ULA par cycle : coupure en milieu de ligne ==="

run "--ula-line" "$TMP/line.ppm"
run "" "$TMP/cell.ppm"

[ -s "$TMP/line.ppm" ] || { ko "capture --ula-line vide"; finish; }
[ -s "$TMP/cell.ppm" ] || { ko "capture par cycle vide"; finish; }

if cmp -s "$TMP/line.ppm" "$TMP/cell.ppm"; then
    ko "les deux rendus sont identiques — le fetch par cycle n'a rien changé"
    finish
fi
ok "les deux rendus diffèrent sur un écran en cours de réécriture"

# Compares the two images line by line. Output: "<differing> <partial>".
set -- $(python3 "$ROOT/tests/integration/ppm_diff_lines.py" "$TMP/line.ppm" "$TMP/cell.ppm")
diff_lines=$1
partial=$2
echo "  lignes différentes : $diff_lines, dont partiellement différentes : $partial"

if [ "$partial" -ge 1 ]; then
    ok "$partial ligne(s) ne diffèrent que sur UNE PARTIE de leur largeur"
    echo "        → l'échantillonnage est intra-ligne : la coupure tombe au milieu"
    echo "          d'une ligne, ce que le rendu ligne-par-ligne ne peut pas produire"
else
    ko "aucune différence partielle : la coupure reste alignée sur les lignes"
fi

finish
