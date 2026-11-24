#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
# tests/integration/test_ci_apt_install.sh
#
# tools/ci_apt_install.sh without network or privileges: a fake `sudo` first in
# PATH simulates an apt that answers, that gets stuck, or that always fails.
# Checks that a stuck attempt is cut and retried, and the give-up after 3 tries.
#
# Author: bmarty <bmarty@mailo.com>
set -u
cd "$(dirname "$0")/../.." || exit 1
command -v timeout >/dev/null 2>&1 || { echo "  SKIP: timeout absent"; exit 0; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
ok() { echo "  PASS: $1"; pass=$((pass + 1)); }
ko() { echo "  FAIL: $1"; fail=$((fail + 1)); }

# Fake sudo: logs the call; MODE=ok | hang1 (1st install stuck) | fail.
cat > "$TMP/sudo" <<'SH'
#!/bin/sh
echo "$*" >> "$LOG"
case "$*" in *" install "*) ;; *) exit 0;; esac
n=$(grep -c " install " "$LOG")
case "$MODE" in
    ok) exit 0;;
    hang1) [ "$n" -eq 1 ] && sleep 30; exit 0;;
    fail) exit 100;;
esac
SH
chmod +x "$TMP/sudo"

run() {   # run MODE → exit code; log in $TMP/MODE.log
    LOG="$TMP/$1.log" MODE=$1 PATH="$TMP:$PATH" CI_APT_T_UPDATE=2 CI_APT_T_INSTALL=2 \
        CI_APT_PAUSE=0 sh tools/ci_apt_install.sh paquet-a paquet-b 2>/dev/null
}
echo "=== ci_apt_install.sh : tentatives bornées et relancées ==="

run ok; rc=$?
if [ $rc -eq 0 ] && [ "$(grep -c ' install ' "$TMP/ok.log")" -eq 1 ] &&
   grep -q "install -y paquet-a paquet-b" "$TMP/ok.log" && grep -q "Acquire::http::Timeout=30" "$TMP/ok.log"; then
    ok "apt qui répond : une tentative, paquets et délai réseau transmis"
else ko "apt qui répond (rc=$rc)"; fi

t0=$(date +%s); run hang1; rc=$?; dt=$(( $(date +%s) - t0 ))
if [ $rc -eq 0 ] && [ "$(grep -c ' install ' "$TMP/hang1.log")" -eq 2 ] && [ $dt -lt 20 ]; then
    ok "install bloqué : coupé à la limite puis relancé avec succès (${dt} s)"
else ko "install bloqué (rc=$rc, ${dt} s)"; fi

run fail; rc=$?
if [ $rc -eq 1 ] && [ "$(grep -c ' install ' "$TMP/fail.log")" -eq 3 ]; then
    ok "apt en échec : abandon après 3 tentatives, code 1"
else ko "apt en échec (rc=$rc)"; fi

echo "=== result: $pass passed, $fail failed ==="
[ $fail -eq 0 ]
