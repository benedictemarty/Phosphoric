#!/bin/sh
# test_docs_claims.sh — garde-fou sur les allégations de précision temporelle.
#
# Le projet a communiqué « cycle-accurate » alors que l'implémentation est au
# niveau N2 (ordonné au cycle bus) — voir docs/ACCURACY.md. Ce test empêche la
# récidive dans les documents de vitrine : toute occurrence de
# « cycle-accurate » / « cycle accurate » doit être qualifiée par « bus- »
# (bus-cycle-accurate), seule formulation autorisée tant que la V2
# (docs/specs/V2_CYCLE_ACCURACY.md) n'a pas livré son harnais de preuve.
#
# Hors périmètre volontairement : ROADMAP/CIRRUS_OS au-delà de l'en-tête et
# docs/CR/** sont des journaux historiques — on ne réécrit pas l'histoire.

ROOT=$(dirname "$0")/../..
cd "$ROOT" || exit 1

pass=0
fail=0

check_file() {
    file="$1"
    lines="$2"   # vide = tout le fichier, sinon nombre de lignes d'en-tête
    [ -f "$file" ] || { echo "FAIL: fichier absent: $file"; fail=$((fail+1)); return; }
    if [ -n "$lines" ]; then
        content=$(head -n "$lines" "$file")
    else
        content=$(cat "$file")
    fi
    # Occurrences NON précédées de "bus-" (insensible à la casse).
    bad=$(printf '%s\n' "$content" \
        | grep -n -i -E 'cycle[- ]accurate' \
        | grep -v -i -E 'bus-cycle-accurate|bus-cycle accurate')
    if [ -n "$bad" ]; then
        echo "FAIL: allégation non qualifiée dans $file :"
        printf '%s\n' "$bad" | sed 's/^/       /'
        echo "       → utiliser « bus-cycle-accurate » + renvoi à docs/ACCURACY.md"
        fail=$((fail+1))
    else
        echo "PASS: $file"
        pass=$((pass+1))
    fi
}

echo "=== Allégations de précision temporelle (docs/ACCURACY.md) ==="
check_file README.md ""
check_file CLAUDE.md ""
check_file ROADMAP 60

# CIRRUS_OS : seule la ligne d'identité du projet est une allégation ; le reste du
# fichier est un journal de build qui peut légitimement CITER le terme fautif.
proj=$(grep '^Project:' CIRRUS_OS 2>/dev/null)
if printf '%s\n' "$proj" | grep -q -i -E 'cycle[- ]accurate' \
   && ! printf '%s\n' "$proj" | grep -q -i -E 'bus-cycle-accurate'; then
    echo "FAIL: allégation non qualifiée dans la ligne Project: de CIRRUS_OS"
    echo "       $proj"
    fail=$((fail+1))
else
    echo "PASS: CIRRUS_OS (ligne Project:)"
    pass=$((pass+1))
fi

# docs/ACCURACY.md doit exister et définir l'échelle : c'est la référence citée.
if [ -f docs/ACCURACY.md ] && grep -q 'N3' docs/ACCURACY.md; then
    echo "PASS: docs/ACCURACY.md définit l'échelle N1→N4"
    pass=$((pass+1))
else
    echo "FAIL: docs/ACCURACY.md manquant ou sans échelle N1→N4"
    fail=$((fail+1))
fi

# Le plan V2 doit exister et être référencé par le ROADMAP.
if [ -f docs/specs/V2_CYCLE_ACCURACY.md ] && grep -q 'V2_CYCLE_ACCURACY.md' ROADMAP; then
    echo "PASS: plan V2 présent et référencé par le ROADMAP"
    pass=$((pass+1))
else
    echo "FAIL: plan V2 absent ou non référencé par le ROADMAP"
    fail=$((fail+1))
fi

echo "---"
echo "Tests passed: $pass"
echo "Tests failed: $fail"
[ "$fail" -eq 0 ]
