#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# tests/integration/test_card_template.sh
#
# « Ajouter une carte = un fichier + une ligne » (ADR 0006, sprint G7) : dans une
# copie de l'arbre courant, on ajoute la carte d'exemple docs/examples/card_demo.c
# (src/cards/card_demo.c) et SA ligne dans include/cards_list.h — rien d'autre —,
# on reconstruit, et on vérifie que la carte est là de bout en bout : aide, option
# et son adresse, ordre des options (préfixe ambigu), menu F1, registre sur le bus,
# section .ost, et qu'une ligne de commande sans la carte reste inchangée.
#
# Author: bmarty <bmarty@mailo.com>
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT=$(pwd)
ROM=roms/basic11b.rom
pass=0
fail=0
ok() { echo "  PASS: $1"; pass=$((pass + 1)); }
ko() { echo "  FAIL: $1"; fail=$((fail + 1)); }

echo "=== Carte d'exemple : un fichier + une ligne suffisent (ADR 0006) ==="
[ -f "$ROM" ] || { echo "  SKIP: $ROM absent"; exit 0; }
command -v git >/dev/null 2>&1 || { echo "  SKIP: git absent"; exit 0; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
W="$T/arbre"
mkdir -p "$W"
# Copie de l'arbre courant (fichiers suivis et nouveaux, sans médias) ; tar
# plutôt que cp --parents, propre à GNU (le runner macOS lance aussi les tests).
git ls-files -co --exclude-standard -z | tar --null -T - -cf - | tar -xf - -C "$W"
mkdir -p "$W/roms"
for r in roms/*.rom; do ln -s "$ROOT/$r" "$W/$r"; done

# Les deux seules modifications.
cp docs/examples/card_demo.c "$W/src/cards/card_demo.c"
awk '{ if ($0 ~ /^    X\(ula_ng, *0\)$/) { print $0 " \\"; print "    X(demo,    1)" } else print }' \
    "$ROOT/include/cards_list.h" > "$W/include/cards_list.h"
if [ "$(diff -r -q "$ROOT/include" "$W/include" | wc -l)" -eq 1 ] &&
   [ "$(diff "$ROOT/include/cards_list.h" "$W/include/cards_list.h" | grep -c '^>')" -le 2 ]; then
    ok "arbre modifié : src/cards/card_demo.c ajouté, une ligne dans include/cards_list.h"
else
    ko "modification inattendue de l'arbre"
fi

echo "  (construction de l'arbre modifié…)"
if ! make -s -C "$W" -j4 > "$T/build.log" 2>&1; then
    ko "construction"; tail -20 "$T/build.log"
    echo "=== result: $pass passed, $fail failed ==="; exit 1
fi
ok "construction sans toucher au Makefile, aux options, au bus ni au menu"
E="$W/oric1-emu"
run() { "$E" -r "$ROM" -n "$@" 2>&1; }

h=$(run --help)
grep -q -- "--demo-addr ADDR" <<<"$h" && ok "aide : --demo et --demo-addr" || ko "aide sans la carte"

o=$(run --demo -c 1000)
grep -q "Demo card enabled at \$03D0" <<<"$o" && ok "--demo : carte en route en \$03D0" || ko "--demo : « $o »"
o=$(run --demo --demo-addr 03D4 -c 1000)
grep -q "Demo card enabled at \$03D4" <<<"$o" && ok "--demo-addr 03D4" || ko "--demo-addr"
o=$(run --dem -c 1000)
grep -q "'--demo' '--demo-addr'" <<<"$o" && ok "--dem : préfixe ambigu, options dans l'ordre de la carte" || ko "--dem : « $o »"

# Registre : écrire $5A, relire $A5 ; puis sauvegarde d'état (section DMO).
# `bread` lit par les périphériques (memory_peek) ; `read` lirait la RAM sous la
# page d'E/S (dbg_peek).
peek03d0() {   # peek03d0 [options] : octet lu en $03D0 après « write 03D0 5A »
    printf 'write 03D0 5A\nbread 03D0 1\nstate-save %s\nquit\n' "$T/demo.ost" |
        "$E" -r "$ROM" -n --control "$@" 2>/dev/null |
        awk '/^OK bread len=1/ { getline; printf "%s", $0; exit }' | od -An -tx1 | tr -d ' \n'
}
b=$(peek03d0 --demo)
[ "$b" = a5 ] && ok "bus : écrire \$5A en \$03D0, relire son complément \$A5" || ko "bus : lu « $b »"
if [ -f "$T/demo.ost" ] && grep -q "DMO" "$T/demo.ost"; then ok "sauvegarde d'état : section « DMO »"; else ko "section DMO absente"; fi
rm -f "$T/demo.ost"
b=$(peek03d0)
[ "$b" != a5 ] && ok "sans --demo : \$03D0 n'appartient pas à la carte (lu « $b »)" || ko "sans --demo, \$03D0 répond quand même"

# Registre du menu F1 et phosphoric.cfg : « carte.demo=oui » et son paramètre
# (clé « demo.adresse », nommée par la fiche du menu) suffisent à l'activer.
printf 'carte.demo=oui\ndemo.adresse=03D8\n' > "$T/demo.cfg"
o=$(run --config "$T/demo.cfg" -c 1000)
grep -q "Demo card enabled at \$03D8" <<<"$o" && ok "menu et phosphoric.cfg : carte.demo=oui, demo.adresse=03D8" || ko "phosphoric.cfg : « $o »"

# Sans la carte, la machine est la même : l'arbre d'origine et l'arbre modifié
# produisent la même sauvegarde d'état.
"$ROOT/oric1-emu" -r "$ROM" -n --save-state "$T/a.ost" -c 300000 >/dev/null 2>&1
"$E" -r "$ROM" -n --save-state "$T/b.ost" -c 300000 >/dev/null 2>&1
cmp -s "$T/a.ost" "$T/b.ost" && ok "sans --demo : .ost identique à l'arbre d'origine" || ko ".ost différent sans la carte"

echo "=== result: $pass passed, $fail failed ==="
[ $fail -eq 0 ]
