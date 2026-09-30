#!/bin/sh
# test_check_skips.sh — self-test of tools/check_skips.sh (make tests-strict).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
pass=0
fail=0
expect() {  # expect <expected rc> <label> <log content>
    printf '%b' "$3" > "$T/log"
    sh "$ROOT/tools/check_skips.sh" "$T/log" "$ROOT/tests/allowed_skips.txt" >/dev/null 2>&1
    rc=$?
    if [ "$rc" = "$1" ]; then echo "PASS: $2"; pass=$((pass+1));
    else echo "FAIL: $2 (rc=$rc, attendu $1)"; fail=$((fail+1)); fi
}
expect 0 "journal sans saut" "PASS: a\nTests passed: 3\n"
expect 0 "ROM absente autorisée" "  test_rom_boot    SKIP (ROM not found)\n"
expect 0 "média du corpus absent autorisé" "Skipped (média absent): 24\n"
expect 0 "nom de test contenant « skipping » ignoré" "  test_readdir_lists_entries_skipping_dotdot   PASS\n"
expect 1 "outil non construit refusé" "  [SKIP] ./bin2tap not built (make tools)\n"
expect 1 "émulateur non construit refusé" "SKIP: ./oric1-emu non construit\n"
expect 1 "dépendance manquante refusée" "  [SKIP] curl not available\n"
expect 1 "un seul saut fautif suffit" "SKIP: ROADMAP absent\n  [SKIP] curl not available\n"
echo "Tests: $((pass+fail)), Passed: $pass, Failed: $fail"
[ "$fail" -eq 0 ]
