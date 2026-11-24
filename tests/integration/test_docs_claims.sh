#!/bin/sh
# test_docs_claims.sh — safeguard on timing-accuracy claims.
#
# The project advertised « cycle-accurate » while the implementation is at
# level N2 (ordered at the bus cycle) — see docs/ACCURACY.md. This test prevents
# a relapse in the showcase documents: every occurrence of
# « cycle-accurate » / « cycle accurate » must be qualified by « bus- »
# (bus-cycle-accurate), the only wording allowed until V2
# (docs/specs/V2_CYCLE_ACCURACY.md) has delivered its proof harness.
#
# Deliberately out of scope: ROADMAP/CIRRUS_OS beyond the header and
# docs/CR/** are historical logs — history is not rewritten.
# The ROADMAP is kept locally (not versioned): it is only checked if it is
# present, a clean checkout (CI) does not have it.

ROOT=$(dirname "$0")/../..
cd "$ROOT" || exit 1

pass=0
fail=0

check_file() {
    file="$1"
    lines="$2"   # empty = whole file, otherwise number of header lines
    [ -f "$file" ] || { echo "FAIL: fichier absent: $file"; fail=$((fail+1)); return; }
    if [ -n "$lines" ]; then
        content=$(head -n "$lines" "$file")
    else
        content=$(cat "$file")
    fi
    # Occurrences NOT preceded by "bus-" (case-insensitive).
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
if [ -f ROADMAP ]; then
    check_file ROADMAP 60
else
    echo "SKIP: ROADMAP absent (tenu en local, non versionné)"
fi

# CIRRUS_OS: only the project identity line is a claim; the rest of the
# file is a build log that may legitimately QUOTE the offending term.
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

# docs/ACCURACY.md must exist and define the scale: it is the cited reference.
if [ -f docs/ACCURACY.md ] && grep -q 'N3' docs/ACCURACY.md; then
    echo "PASS: docs/ACCURACY.md définit l'échelle N1→N4"
    pass=$((pass+1))
else
    echo "FAIL: docs/ACCURACY.md manquant ou sans échelle N1→N4"
    fail=$((fail+1))
fi

# The V2 plan must exist and be referenced by docs/ACCURACY.md (versioned).
if [ -f docs/specs/V2_CYCLE_ACCURACY.md ] && grep -q 'V2_CYCLE_ACCURACY.md' docs/ACCURACY.md; then
    echo "PASS: plan V2 présent et référencé par docs/ACCURACY.md"
    pass=$((pass+1))
else
    echo "FAIL: plan V2 absent ou non référencé par docs/ACCURACY.md"
    fail=$((fail+1))
fi

echo "---"
echo "Tests passed: $pass"
echo "Tests failed: $fail"
[ "$fail" -eq 0 ]
