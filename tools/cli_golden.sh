#!/bin/sh
# cli_golden.sh — comparaison différentielle de deux binaires sur un corpus de
# lignes de commande (garde-fou des refactors de main()/du parseur).
#
# Usage : tools/cli_golden.sh ANCIEN NOUVEAU [CORPUS]
#   CORPUS (défaut tests/cli_golden/cases.txt) : une ligne d'arguments par cas,
#   lue par le shell (guillemets permis) ; « @OUT@ » = répertoire de sortie
#   propre au cas et au binaire. Lignes vides et « # » ignorées.
# Pour chaque cas, les deux binaires tournent depuis la racine du dépôt ; on
# compare code de retour, stdout, stderr (horodatages et @OUT@ normalisés) et
# chaque fichier produit sous @OUT@. Code de sortie 1 au premier écart signalé
# (tous les cas sont quand même joués). GOLDEN_KEEP=1 garde les sorties.
set -u
OLD=${1:?usage: cli_golden.sh ANCIEN NOUVEAU [CORPUS]}
NEW=${2:?usage: cli_golden.sh ANCIEN NOUVEAU [CORPUS]}
CASES=${3:-tests/cli_golden/cases.txt}
T=$(mktemp -d) || exit 1
[ "${GOLDEN_KEEP:-0}" = 1 ] && echo "sorties conservées : $T" || trap 'rm -rf "$T"' EXIT

norm() {  # norm <fichier> <répertoire @OUT@>
    sed -e "s|$2|@OUT@|g" \
        -e 's/\[[0-9]\{4\}-[0-9][0-9]-[0-9][0-9] [0-9][0-9]:[0-9][0-9]:[0-9][0-9]\]/[TS]/g' "$1" |
    sed -E '/^BENCH /s/(wall_ms|mhz_eq|speed_ratio|frame_us)=[0-9.x]*/\1=N/g'
}

# `timeout` (coreutils) est absent de macOS par défaut : sans lui, pas de limite.
TO=""
command -v timeout >/dev/null 2>&1 && TO="timeout ${GOLDEN_TIMEOUT:-60}"

# Les deux binaires tournent sous le MÊME chemin (lien symbolique) : argv[0]
# apparaît dans l'aide et dans certains messages.
BIN="$T/oric1-emu"
abs() { case "$1" in /*) echo "$1" ;; *) echo "$(pwd)/$1" ;; esac; }
OLD=$(abs "$OLD"); NEW=$(abs "$NEW")

run_one() {  # run_one <binaire> <répertoire> <arguments>
    mkdir -p "$2/out"
    ln -sf "$1" "$BIN"
    args=$(printf '%s' "$3" | sed "s|@OUT@|$2/out|g")
    eval "$TO \"\$BIN\" $args" </dev/null >"$2/stdout" 2>"$2/stderr"
    echo $? > "$2/rc"
}

n=0; bad=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|'#'*) continue ;; esac
    n=$((n+1))
    a="$T/$n/a"; b="$T/$n/b"
    run_one "$OLD" "$a" "$line"
    run_one "$NEW" "$b" "$line"
    diffs=""
    cmp -s "$a/rc" "$b/rc" || diffs="$diffs rc($(cat "$a/rc")→$(cat "$b/rc"))"
    for s in stdout stderr; do
        norm "$a/$s" "$a/out" > "$a/$s.n"; norm "$b/$s" "$b/out" > "$b/$s.n"
        cmp -s "$a/$s.n" "$b/$s.n" || diffs="$diffs $s"
    done
    (cd "$a/out" && find . -type f | sort) > "$a/files"
    (cd "$b/out" && find . -type f | sort) > "$b/files"
    if cmp -s "$a/files" "$b/files"; then
        while IFS= read -r f; do
            cmp -s "$a/out/$f" "$b/out/$f" || diffs="$diffs fichier:$f"
        done < "$a/files"
    else
        diffs="$diffs liste-fichiers"
    fi
    if [ -n "$diffs" ]; then
        bad=$((bad+1)); echo "DIFF [$n] $line"; echo "      →$diffs"
    fi
done < "$CASES"
echo "--- $n cas, $bad écart(s)"
[ "$bad" -eq 0 ]
