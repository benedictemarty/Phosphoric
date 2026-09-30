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

expect 0 "arbre identique accepté"

sed -i 's|/\* Initialise le compteur|/* Initialises the frame\n * counter (one more line)|; s|// valeur de départ|// start value|' a.c
sed -i 's|# Lance le test|# Runs the test|; s|# vrai commentaire|# real comment|' b.sh
sed -i 's|# Calcule la somme|# Computes the sum|; s|# commentaire|# comment|' c.py
sed -i 's|# fin|# end|; s|# règle par défaut|# default rule|' Makefile
expect 0 "commentaires traduits (C, sh, py, Makefile) acceptés"

sed -i 's|Disque inséré|Disk inserted|' a.c
expect 1 "chaîne C modifiée refusée"

sed -i 's|int n = 0|int n = 1|' a.c
expect 1 "code C modifié refusé"

sed -i 's|# pas un commentaire|# not a comment|' b.sh
expect 1 "« # » dans une chaîne shell modifiée refusé"

sed -i 's|"texte"|"text"|' c.py
expect 1 "chaîne Python modifiée refusée"

sed -i 's|@echo ok|@echo OK|' Makefile
expect 1 "commande Makefile modifiée refusée"

echo "Tests: $((pass+fail)), Passed: $pass, Failed: $fail"
[ "$fail" -eq 0 ]
