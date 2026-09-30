#!/bin/sh
# test_cli_golden.sh — self-test of tools/cli_golden.sh.
# A binary compared with itself: no difference. A "mutant" (same binary, one
# more line on stdout): difference detected. Needs no ROM (--help).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EMU="$ROOT/${EMU:-oric1-emu}"
[ -x "$EMU" ] || { echo "SKIP: $EMU non construit"; exit 0; }
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
printf -- '--help\n# commentaire ignoré\n\n--version-inexistante\n' > "$T/cases.txt"
printf '#!/bin/sh\n"%s" "$@"\nrc=$?\necho mutation\nexit $rc\n' "$EMU" > "$T/mutant"
chmod +x "$T/mutant"
pass=0; fail=0
cd "$ROOT" || exit 1
if sh tools/cli_golden.sh "$EMU" "$EMU" "$T/cases.txt" >/dev/null 2>&1; then
    echo "PASS: binaire identique à lui-même"; pass=$((pass+1))
else echo "FAIL: écart signalé entre un binaire et lui-même"; fail=$((fail+1)); fi
out=$(sh tools/cli_golden.sh "$EMU" "$T/mutant" "$T/cases.txt" 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -q -- '--- 2 cas, 2 écart(s)'; then
    echo "PASS: mutant détecté sur les 2 cas"; pass=$((pass+1))
else echo "FAIL: mutant non détecté : $out"; fail=$((fail+1)); fi
echo "Tests: $((pass+fail)), Passed: $pass, Failed: $fail"
[ "$fail" -eq 0 ]
