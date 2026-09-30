#!/bin/sh
# test_comment_only_diff.sh — auto-test de tools/check_comment_only_diff.py.
#
# L'outil garde la branche miroir anglaise `main-en` identique à `main` hors
# commentaires. On le vérifie dans un dépôt git jetable : un commentaire traduit
# (même sur un autre nombre de lignes) doit passer ; une chaîne, un identifiant
# ou une commande modifiés doivent échouer, pour chaque famille de fichiers.
set -u
TOOL="$(cd "$(dirname "$0")/../.." && pwd)/tools/check_comment_only_diff.py"
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
pass=0
fail=0

cd "$T" || exit 1
git init -q -b main . && git config user.email t@t && git config user.name t
cat > a.c <<'EOF'
/* Initialise le compteur
 * de trames. */
int n = 0; // valeur de départ
const char *m = "Disque inséré";
EOF
cat > b.sh <<'EOF'
#!/bin/sh
# Lance le test
echo "# pas un commentaire"  # vrai commentaire
EOF
cat > c.py <<'EOF'
# Calcule la somme
x = "texte"  # commentaire
EOF
printf 'all:\n\t@echo ok # fin\n# règle par défaut\n' > Makefile
git add -A && git commit -q -m base

expect() {  # expect <rc attendu> <libellé>
    if python3 "$TOOL" a.c b.sh c.py Makefile >/dev/null 2>&1; then rc=0; else rc=1; fi
    if [ "$rc" = "$1" ]; then echo "PASS: $2"; pass=$((pass+1));
    else echo "FAIL: $2 (rc=$rc, attendu $1)"; fail=$((fail+1)); fi
    git checkout -q -- .
}

# Remplacement littéral portable : le `sed -i` de BSD/macOS n'a pas la syntaxe
# GNU (il prendrait le script pour un suffixe et ne modifierait rien).
edit() {  # edit <fichier> <ancien> <nouveau>
    python3 - "$@" <<'PY' || echo "edit impossible : $2" >&2
import sys
p, a, b = sys.argv[1:4]
s = open(p, encoding="utf-8").read()
assert a in s
open(p, "w", encoding="utf-8").write(s.replace(a, b, 1))
PY
}
NL='
'

expect 0 "arbre identique accepté"

edit a.c '/* Initialise le compteur' "/* Initialises the frame${NL} * counter (one more line)"
edit a.c '// valeur de départ' '// start value'
edit b.sh '# Lance le test' '# Runs the test'
edit b.sh '# vrai commentaire' '# real comment'
edit c.py '# Calcule la somme' '# Computes the sum'
edit c.py '  # commentaire' '  # comment'
edit Makefile '# fin' '# end'
edit Makefile '# règle par défaut' '# default rule'
if git diff --quiet; then
    echo "FAIL: les modifications de commentaires n'ont pas été appliquées"; fail=$((fail+1))
fi
expect 0 "commentaires traduits (C, sh, py, Makefile) acceptés"

edit a.c 'Disque inséré' 'Disk inserted'
expect 1 "chaîne C modifiée refusée"

edit a.c 'int n = 0' 'int n = 1'
expect 1 "code C modifié refusé"

edit b.sh '# pas un commentaire' '# not a comment'
expect 1 "« # » dans une chaîne shell modifiée refusé"

edit c.py '"texte"' '"text"'
expect 1 "chaîne Python modifiée refusée"

edit Makefile '@echo ok' '@echo OK'
expect 1 "commande Makefile modifiée refusée"

echo "Tests: $((pass+fail)), Passed: $pass, Failed: $fail"
[ "$fail" -eq 0 ]
