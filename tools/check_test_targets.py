#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""check_test_targets.py — toute cible test-* du Makefile est dans `make tests`.

Usage : tools/check_test_targets.py Makefile tests/out_of_suite.txt

Une cible test-* qui n'est ni une dépendance de `tests:` ni listée (avec sa
raison) dans out_of_suite.txt ne tourne jamais : test-serial-backends l'a été
de 2.11.1 à 2.12.6. Code de sortie 1 dans ce cas, ou si une exemption est
périmée (cible inexistante, ou déjà dans la suite) ou sans raison.

Author: bmarty <bmarty@mailo.com>
"""
import re
import sys


def main(makefile, exempt_file):
    mk = open(makefile, encoding="utf-8").read()
    targets = set(re.findall(r"^(test-[a-z0-9-]+)\s*:", mk, re.M))
    targets |= set(re.findall(r"call (?:UNIT_TEST|DIRECT_TEST),(test-[a-z0-9-]+),", mk))
    m = re.search(r"^tests:(.*)$", mk, re.M)
    if not m:
        print("check_test_targets : cible `tests:` introuvable")
        return 1
    suite = set(m.group(1).split())

    exempt, errors = {}, []
    for n, line in enumerate(open(exempt_file, encoding="utf-8"), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, why = line.partition(" ")
        if not why.strip():
            errors.append(f"{exempt_file}:{n} : {name} sans raison")
        exempt[name] = why.strip()

    for t in sorted(targets - suite - set(exempt)):
        errors.append(f"{t} : hors de `make tests` et non exemptée ({exempt_file})")
    for t in sorted(exempt):
        if t not in targets:
            errors.append(f"{t} : exemptée mais n'existe pas (exemption périmée)")
        elif t in suite:
            errors.append(f"{t} : exemptée mais déjà dans `make tests` (exemption périmée)")

    print("=== Cibles de test : toutes dans `make tests` ou exemptées ===")
    for e in errors:
        print("  FAIL: " + e)
    print(f"  {len(targets)} cibles test-*, {len(targets & suite)} dans la suite, "
          f"{len(exempt)} exemptée(s)")
    print(f"=== result: {0 if errors else 1} passed, {len(errors)} failed ===")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
