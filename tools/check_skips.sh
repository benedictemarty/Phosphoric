#!/bin/sh
# check_skips.sh — refuse les tests sautés sans raison autorisée.
#
# Usage : tools/check_skips.sh JOURNAL [LISTE]
# Relève dans JOURNAL (sortie de `make tests`) chaque ligne de saut (SKIP,
# Skipped, « — skipping ») et la confronte aux expressions de LISTE
# (défaut : tests/allowed_skips.txt). Code de sortie 1 si un saut n'est pas couvert.
set -u
LOG=${1:?usage: check_skips.sh JOURNAL [LISTE]}
ALLOW=${2:-tests/allowed_skips.txt}
PATTERNS=$(mktemp) || exit 1
trap 'rm -f "$PATTERNS"' EXIT
grep -v -e '^#' -e '^[[:space:]]*$' "$ALLOW" > "$PATTERNS"

skips=$(grep -E '(^|[^_[:alnum:]])(SKIP|Skipped)([^_[:alnum:]]|$)|— skipping' "$LOG")
total=$(printf '%s\n' "$skips" | grep -c .)
bad=$(printf '%s\n' "$skips" | grep . | grep -v -E -f "$PATTERNS")
if [ -n "$bad" ]; then
    echo "FAIL: saut(s) de test non autorisé(s) :"
    printf '%s\n' "$bad" | sed 's/^/       /'
    echo "       → construire ce qui manque, ou justifier le saut dans $ALLOW"
    exit 1
fi
echo "PASS: $total saut(s), tous autorisés ($ALLOW)"
