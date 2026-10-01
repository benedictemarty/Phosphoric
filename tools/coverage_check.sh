#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
#
# coverage_check.sh — couverture des lignes de src/ et seuil qui ne descend pas.
#
# Usage : tools/coverage_check.sh BUILD [FICHIER_SEUIL]
#   BUILD          dossier de build instrumenté (make coverage, COVERAGE=1)
#   FICHIER_SEUIL  pourcentage minimal (défaut tests/coverage_floor.txt)
#
# Échoue si la couverture totale de src/*.c passe sous le seuil. Quand elle le
# dépasse d'au moins un point, propose la nouvelle valeur : le seuil se relève
# à la main, dans le même commit que les tests qui l'ont fait monter (cliquet).
# Portable (sh + awk + gcov), sans gcovr ni lcov.
#
# Author: bmarty <bmarty@mailo.com>
set -eu
BUILD=${1:?usage: coverage_check.sh BUILD [FICHIER_SEUIL]}
FLOOR_FILE=${2:-tests/coverage_floor.txt}
FLOOR=$(grep -v '^#' "$FLOOR_FILE" | head -1 | tr -d ' \r')

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
find "$BUILD/src" -name '*.gcno' | while read -r g; do
    src=${g#"$BUILD"/}
    src=${src%.gcno}.c
    [ -f "$src" ] || continue
    gcov -n -o "$(dirname "$g")" "$src" 2>/dev/null || true
done > "$tmp"

awk -v floor="$FLOOR" '
    /^File / {
        f = $0; sub(/^File .?/, "", f); sub(/.$/, "", f)
        keep = (f ~ /^src\/.*\.c$/) && !(f in seen)
        next
    }
    /^Lines executed:/ && keep {
        s = $0; sub(/^Lines executed:/, "", s)
        split(s, a, "% of ")
        pct = a[1] + 0; n = a[2] + 0
        seen[f] = 1; cnt[f] = n; cov[f] = pct
        tl += n; te += n * pct / 100; files++
        keep = 0
    }
    END {
        if (tl == 0) { print "coverage_check : aucune donnée gcov (make coverage ?)"; exit 2 }
        total = te / tl * 100
        printf "Couverture de src/*.c : %.1f %% (%d/%d lignes, %d fichiers) — seuil %.1f %%\n",
               total, te, tl, files, floor
        # Les moins couverts parmi les fichiers de plus de 100 lignes.
        print "Fichiers les moins couverts (> 100 lignes) :"
        for (k = 0; k < 8; k++) {
            best = ""; bp = 101
            for (f in cov) if (cnt[f] > 100 && !(f in shown) && cov[f] < bp) { bp = cov[f]; best = f }
            if (best == "") break
            shown[best] = 1
            printf "  %5.1f %%  %5d  %s\n", cov[best], cnt[best], best
        }
        if (total + 0.05 < floor) {
            printf "ÉCHEC : couverture %.1f %% sous le seuil %.1f %%\n", total, floor
            exit 1
        }
        if (total >= floor + 1)
            printf "Le seuil peut monter : %d (tests/coverage_floor.txt)\n", int(total)
        print "PASS"
    }' "$tmp"
