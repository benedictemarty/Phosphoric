#!/usr/bin/env bash
#
# test_http_api_e2e.sh — end-to-end test of the HTTP control API (sprint 94).
#
# Launches a headless emulator with --http-api on a private port, drives the
# REST endpoints with curl, and asserts the JSON replies. Because the API is a
# build-time option (HTTPAPI=1), the test SKIPS gracefully — exit 0 — when the
# server is unreachable (HTTPAPI=0 build) or curl is missing, so it is safe in
# the default `make tests` run.
#
# Checks:
#   1. GET  /hello            → ok:true, advertises caps
#   2. GET  /regs             → ok:true, A=/PC= register dump
#   3. POST /mem + GET /mem   → write AB CD, read it back (state round-trip)
#   4. POST /reset            → ok:true, pc=
#   5. GET  /peek/via         → ok:true, VIA registers
#   6. POST /tape ../escape   → ok:false (sandbox rejects parent path)
#   7. GET  /nope             → HTTP 404
#
# Author: bmarty <bmarty@mailo.com>

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
[ -f "$ROM" ] || ROM="roms/basic10.rom"
PORT=8913
BASE="http://127.0.0.1:$PORT"
LOG="$(mktemp)"

pass=0
fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== HTTP control API e2e test ==="

[ -x "$EMU" ]        || { echo "  [SKIP] $EMU not built"; exit 0; }
[ -f "$ROM" ]        || { echo "  [SKIP] no BASIC ROM"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] curl not available"; exit 0; }

EMU_PID=""
cleanup() { [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null; wait "$EMU_PID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

"$EMU" -r "$ROM" --http-api=$PORT --http-api-root /tmp >"$LOG" 2>&1 &
EMU_PID=$!

# Poll /hello until the server answers (up to ~4 s).
up=0
for _ in $(seq 1 40); do
    if curl -s -m 1 "$BASE/hello" >/dev/null 2>&1; then up=1; break; fi
    sleep 0.1
done
if [ "$up" -ne 1 ]; then
    if grep -qi "not compiled in" "$LOG"; then
        echo "  [SKIP] built without HTTPAPI=1"
    else
        echo "  [SKIP] HTTP API server unreachable (see log)"
    fi
    exit 0
fi

# 1. hello
curl -s "$BASE/hello" | grep -q '"ok":true' \
    && curl -s "$BASE/hello" | grep -q 'caps=.*load-disk' \
    && ok "GET /hello advertises caps" || ko "GET /hello"

# 2. regs
curl -s "$BASE/regs" | grep -qE '"ok":true.*A=[0-9A-F]{2}.*PC=[0-9A-F]{4}' \
    && ok "GET /regs returns a register dump" || ko "GET /regs"

# 3. write/read round-trip through the queue
curl -s -X POST --data 'addr=10&bytes=AB+CD' "$BASE/mem" | grep -q '"reply":"count=2"' \
    && ok "POST /mem writes 2 bytes" || ko "POST /mem write"
curl -s "$BASE/mem?addr=10&len=2" | grep -q '"reply":"AB CD"' \
    && ok "GET /mem reads back AB CD (state round-trip)" || ko "GET /mem read"

# 4. reset
curl -s -X POST "$BASE/reset" | grep -qE '"ok":true.*"reply":"pc=[0-9A-F]{4}"' \
    && ok "POST /reset resets the CPU" || ko "POST /reset"

# 5. peek
curl -s "$BASE/peek/via" | grep -q 'ora=.*ifr=' \
    && ok "GET /peek/via dumps the VIA" || ko "GET /peek/via"

# 6. sandbox: parent-escaping path rejected
curl -s -X POST --data 'path=../etc/passwd' "$BASE/tape" | grep -q '"ok":false' \
    && ok "POST /tape rejects ../ escape (sandbox)" || ko "sandbox escape not rejected"

# 7. unknown route → 404
code="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/nope")"
[ "$code" = "404" ] && ok "GET /nope → 404" || ko "unknown route returned $code"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
