#!/usr/bin/env bash
#
# test_web_iomenu.sh — menu des périphériques (F1) dans la build WebAssembly
#
# Construit la build web (make wasm, emsdk requis), la sert en HTTP local et la
# pilote dans Chrome headless (Playwright) : F1 ouvre le menu sans atteindre le
# navigateur, machine figée pendant le menu, clavier virtuel qui le pilote,
# bouton I/O de la barre latérale.
#
# SKIP (exit 0) si emcc, node, Playwright ou Chrome sont absents : outils
# optionnels, hors CI par défaut.
set -u
cd "$(dirname "$0")/../.."

skip() { echo "SKIP test-web-iomenu: $*"; exit 0; }

if ! command -v emcc >/dev/null 2>&1 && [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    # shellcheck disable=SC1091
    source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
command -v emcc >/dev/null 2>&1 || skip "emcc introuvable (emsdk)"
command -v node >/dev/null 2>&1 || skip "node introuvable"
command -v python3 >/dev/null 2>&1 || skip "python3 introuvable"

echo "test-web-iomenu: make wasm…"
make -s wasm >/dev/null 2>&1 || { echo "FAIL: make wasm"; exit 1; }

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory web >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/phosphoric.html" && break; sleep 0.1; done

node tests/integration/web_iomenu_e2e.js "http://127.0.0.1:$PORT"
rc=$?
[ $rc -eq 77 ] && skip "Playwright/Chrome indisponible"
[ $rc -eq 0 ] && echo "test-web-iomenu: OK" || echo "test-web-iomenu: ÉCHEC"
exit $rc
