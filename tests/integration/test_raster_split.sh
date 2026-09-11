#!/bin/sh
# test_raster_split.sh — preuve que l'ULA fetche au cycle (V2-E4 / US4.4).
#
# Un petit programme machine réécrit en boucle la mémoire écran en alternant deux
# octets d'ATTRIBUT : $10 (PAPER noir) et $16 (PAPER cyan). L'écran est donc en
# permanente réécriture pendant que le faisceau le balaie.
#
#   --ula-line  (comportement d'avant la V2) : chaque scanline est échantillonnée
#               à un instant unique, celui où elle s'achève.
#   défaut      (fetch d'une cellule par cycle) : chaque cellule voit la mémoire à
#               l'instant exact de son propre fetch.
#
# Critère : il existe des lignes qui ne diffèrent entre les deux rendus que SUR
# UNE PARTIE de leur largeur. Une différence partielle prouve que l'échantillonnage
# est intra-ligne — une coupure au milieu d'une ligne, ce que le rendu
# ligne-par-ligne ne peut structurellement pas produire. C'est l'effet « raster
# split » exploité par les démos Oric.
#
# Le cas déterministe et exact vit dans `make test-clock`
# (test_ula_per_cycle_mid_line_split) ; ce test-ci est la preuve sur du vrai code
# 6502 exécuté par l'émulateur complet.

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

# ── Le programme, assemblé à la main (32 octets à $0500) ──
#   0500  A9 16        LDA #$16          ; attribut PAPER cyan
#   0502  A2 00        LDX #$00          ; <- boucle
#   0504  9D 80 BB     STA $BB80,X
#   0507  9D 80 BC     STA $BC80,X
#   050A  9D 80 BD     STA $BD80,X
#   050D  9D 80 BE     STA $BE80,X
#   0510  E8           INX
#   0511  D0 F1        BNE $0504
#   0513  A2 60        LDX #$60
#   0515  CA           DEX               ; <- queue $BF80-$BFDF
#   0516  9D 80 BF     STA $BF80,X
#   0519  D0 FA        BNE $0515
#   051B  49 06        EOR #$06          ; bascule $16 <-> $10
#   051D  4C 02 05     JMP $0502
printf '\251\026\242\000\235\200\273\235\200\274\235\200\275\235\200\276\350\320\361\242\140\312\235\200\277\320\372\111\006\114\002\005' \
    >"$TMP/split.bin"
[ "$(wc -c <"$TMP/split.bin")" = "32" ] || { ko "binaire de test mal formé"; finish; }

./bin2tap "$TMP/split.bin" --start 0x0500 --exec 0x0500 -o "$TMP/split.tap" \
    --name SPLIT >/dev/null 2>&1 || { ko "bin2tap a échoué"; finish; }

run() { # $1 = options, $2 = fichier PPM
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

# Compare les deux images ligne par ligne. Sortie : "<différentes> <partielles>".
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
