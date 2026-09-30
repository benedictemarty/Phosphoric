#!/usr/bin/env bash
#
# test_web_loci.sh — e2e of the LOCI cartridge in the WebAssembly build
#
# Builds the web build (make wasm, emsdk required), serves it over local HTTP and
# drives it in headless Chrome (Playwright): LOCI menu at boot via ?loci=1,
# seeded persistent IDBFS flash, file import, persistence across reload,
# file listed by the menu's selector, and — if disks/3dfongus.dsk exists
# locally — mounting as A: then booting the disk.
#
# SKIP (exit 0) if emcc, node, Playwright or Chrome are missing: optional
# tools, outside CI by default.
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
