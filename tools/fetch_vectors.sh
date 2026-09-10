#!/bin/sh
# fetch_vectors.sh — récupère les jeux de vecteurs d'oracle CPU (V2-S1).
#
# Rien de tout cela n'est versionné : ce sont des dépôts tiers volumineux
# (~1 Go pour 65x02) sous leurs propres licences. Les tests `make test-cycle`
# et `make test-dormann` se mettent en SKIP quand les vecteurs sont absents.
#
#   1. SingleStepTests/65x02 — 10 000 cas par opcode, avec la trace bus
#      cycle par cycle attendue (256 fichiers JSON).
#   2. Klaus2m5/6502_65C02_functional_tests — test fonctionnel 6502 (+ son
#      listing, qui donne l'adresse de succès). Le test décimal n'est publié
#      qu'en source .a65 (pas de binaire) : il faudrait l'assembler avec as65,
#      hors périmètre — le test fonctionnel couvre déjà le mode décimal.
#
# Usage :
#   tools/fetch_vectors.sh              # tout (65x02 + Dormann)
#   tools/fetch_vectors.sh dormann      # seulement Dormann (~150 Ko)
#   tools/fetch_vectors.sh 65x02 a9 b1  # seulement ces opcodes

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/third_party/vectors"
V65="$DEST/65x02"
DORM="$DEST/dormann"
BASE65="https://raw.githubusercontent.com/SingleStepTests/65x02/main/6502/v1"
BASEDORM="https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files"

command -v curl >/dev/null 2>&1 || { echo "curl requis" >&2; exit 1; }

fetch() { # url dest
    [ -s "$2" ] && return 0
    curl -sSfL --retry 3 --retry-delay 2 -o "$2.part" "$1" || { rm -f "$2.part"; return 1; }
    mv "$2.part" "$2"
}

fetch_dormann() {
    mkdir -p "$DORM" || exit 1
    for f in 6502_functional_test.bin 6502_functional_test.lst; do
        printf '  %s ... ' "$f"
        if fetch "$BASEDORM/$f" "$DORM/$f"; then echo "ok"; else echo "ÉCHEC"; fi
    done
}

fetch_65x02() {
    mkdir -p "$V65" || exit 1
    if [ "$#" -gt 0 ]; then
        list="$*"
    else
        list=$(awk 'BEGIN{for(i=0;i<256;i++) printf "%02x\n", i}')
    fi
    n=0; ok=0; skip=0
    for op in $list; do
        n=$((n+1))
        if [ -s "$V65/$op.json" ]; then skip=$((skip+1)); continue; fi
        if fetch "$BASE65/$op.json" "$V65/$op.json"; then
            ok=$((ok+1))
        else
            # 26 opcodes JAM/KIL n'ont pas de fichier en amont : c'est normal.
            echo "  (absent en amont: $op)"
        fi
        [ $((n % 32)) -eq 0 ] && echo "  ... $n/256"
    done
    echo "  65x02 : $ok téléchargés, $skip déjà présents, sur $n demandés"
}

what=${1:-all}
[ "$#" -gt 0 ] && shift
case "$what" in
    dormann) echo "Vecteurs Dormann → $DORM"; fetch_dormann ;;
    65x02)   echo "Vecteurs 65x02 → $V65";   fetch_65x02 "$@" ;;
    all)     echo "Vecteurs Dormann → $DORM"; fetch_dormann
             echo "Vecteurs 65x02 → $V65 (~1 Go, une seule fois)"; fetch_65x02 ;;
    *)       echo "usage: $0 [all|65x02 [op...]|dormann]" >&2; exit 1 ;;
esac
echo "Terminé. $DEST"
