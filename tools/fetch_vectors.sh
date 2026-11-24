#!/bin/sh
# fetch_vectors.sh — fetches the CPU oracle vector sets (V2-S1).
#
# None of this is version-controlled: these are large third-party repositories
# (~1 GB for 65x02) under their own licences. The `make test-cycle` and
# `make test-dormann` tests SKIP when the vectors are missing.
#
#   1. SingleStepTests/65x02 — 10,000 cases per opcode, with the expected
#      cycle-by-cycle bus trace (256 JSON files).
#   2. Klaus2m5/6502_65C02_functional_tests — 6502 functional test (+ its
#      listing, which gives the success address). The decimal test is only
#      published as .a65 source (no binary): it would have to be assembled with
#      as65, out of scope — the functional test already covers decimal mode.
#
# Usage:
#   tools/fetch_vectors.sh              # everything (65x02 + Dormann)
#   tools/fetch_vectors.sh dormann      # Dormann only (~150 KB)
#   tools/fetch_vectors.sh 65x02 a9 b1  # only these opcodes

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
            # 26 JAM/KIL opcodes have no upstream file: this is expected.
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
