#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# tests/integration/test_card_template.sh
#
# "Adding a card = one file + one line" (ADR 0006, sprint G7): in a copy of the
# current tree, we add the sample card docs/examples/card_demo.c
# (src/cards/card_demo.c) and ITS line in include/cards_list.h — nothing else —,
# rebuild, and check that the card is there end to end: help, option and its
# address, option order (ambiguous prefix), F1 menu, register on the bus,
# .ost section, and that a command line without the card stays unchanged.
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
# Copy of the current tree (tracked and new files, no media); tar rather
# than cp --parents, which is GNU-specific (the macOS runner also runs the tests).
git ls-files -co --exclude-standard -z | tar --null -T - -cf - | tar -xf - -C "$W"
mkdir -p "$W/roms"
for r in roms/*.rom; do ln -s "$ROOT/$r" "$W/$r"; done

# The only two changes.
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

# Register: write $5A, read back $A5; then save state (DMO section).
# `bread` reads through the peripherals (memory_peek); `read` would read the RAM
# under the I/O page (dbg_peek).
peek03d0() {   # peek03d0 [options]: byte read at $03D0 after "write 03D0 5A"
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

# F1 menu and phosphoric.cfg: "carte.demo=oui" and its parameter
# (key "demo.adresse", named by the menu entry) are enough to enable it.
printf 'carte.demo=oui\ndemo.adresse=03D8\n' > "$T/demo.cfg"
o=$(run --config "$T/demo.cfg" -c 1000)
grep -q "Demo card enabled at \$03D8" <<<"$o" && ok "menu et phosphoric.cfg : carte.demo=oui, demo.adresse=03D8" || ko "phosphoric.cfg : « $o »"

# Without the card, the machine is the same: the original tree and the modified
# tree produce the same save state.
"$ROOT/oric1-emu" -r "$ROM" -n --save-state "$T/a.ost" -c 300000 >/dev/null 2>&1
"$E" -r "$ROM" -n --save-state "$T/b.ost" -c 300000 >/dev/null 2>&1
cmp -s "$T/a.ost" "$T/b.ost" && ok "sans --demo : .ost identique à l'arbre d'origine" || ko ".ost différent sans la carte"

echo "=== result: $pass passed, $fail failed ==="
[ $fail -eq 0 ]
