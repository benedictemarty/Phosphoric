#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""check_test_targets.py — every test-* target of the Makefile is in `make tests`.

Usage: tools/check_test_targets.py Makefile tests/out_of_suite.txt

A test-* target that is neither a dependency of `tests:` nor listed (with its
reason) in out_of_suite.txt never runs: test-serial-backends was such a target
from 2.11.1 to 2.12.6. Exit code 1 in that case, or if an exemption is stale
(target missing, or already in the suite) or has no reason.

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
