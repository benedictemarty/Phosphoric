#!/usr/bin/env bash
#
# test_web_loci.sh — e2e de la cartouche LOCI dans la build WebAssembly
#
# Construit la build web (make wasm, emsdk requis), la sert en HTTP local et la
# pilote dans Chrome headless (Playwright) : menu LOCI au boot via ?loci=1,
# flash persistant IDBFS semé, import d'un fichier, persistance au rechargement,
# fichier listé par le sélecteur du menu, et — si disks/3dfongus.dsk existe
# localement — montage en A: puis démarrage du disque.
#
# SKIP (exit 0) si emcc, node, Playwright ou Chrome sont absents : outils
# optionnels, hors CI par défaut.
set -u
cd "$(dirname "$0")/../.."

skip() { echo "SKIP test-web-loci: $*"; exit 0; }

if ! command -v emcc >/dev/null 2>&1 && [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    # shellcheck disable=SC1091
    source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
command -v emcc >/dev/null 2>&1 || skip "emcc introuvable (emsdk)"
command -v node >/dev/null 2>&1 || skip "node introuvable"
command -v python3 >/dev/null 2>&1 || skip "python3 introuvable"

echo "test-web-loci: make wasm…"
make -s wasm >/dev/null 2>&1 || { echo "FAIL: make wasm"; exit 1; }

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory web >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/phosphoric.html" && break; sleep 0.1; done

DISK=""
[ -f disks/3dfongus.dsk ] && DISK="disks/3dfongus.dsk"

node tests/integration/web_loci_e2e.js "http://127.0.0.1:$PORT" $DISK
rc=$?
[ $rc -eq 77 ] && skip "Playwright/Chrome indisponible"
[ $rc -eq 0 ] && echo "test-web-loci: OK" || echo "test-web-loci: ÉCHEC"
exit $rc
