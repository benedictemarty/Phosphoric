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

# /hello answers as soon as the HTTP thread is up — which is *before* the ROM
# finishes its power-on RAM clear. Let the machine settle so memory writes stick.
sleep 2

# 3. write/read round-trip through the queue. Target free RAM ($9000, untouched
#    by the ROM at the Ready prompt) — zero-page ($10) races the live interpreter.
curl -s -X POST --data 'addr=9000&bytes=AB+CD' "$BASE/mem" | grep -q '"reply":"count=2"' \
    && ok "POST /mem writes 2 bytes" || ko "POST /mem write"
curl -s "$BASE/mem?addr=9000&len=2" | grep -q '"reply":"AB CD"' \
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

# 8. keyboard injection (Epic 4): type a BASIC line remotely and verify it runs.
#    `\n` (backslash-n) is translated to RETURN by the `keys` command. Give the
#    ROM time to reach Ready, then to type + execute the line.
sleep 3
curl -s -X POST --data-urlencode 'text=POKE 4096,123\n' "$BASE/keys" | grep -q '"ok":true' \
    && ok "POST /keys accepts keystrokes" || ko "POST /keys"
typed=0
for _ in $(seq 1 15); do
    sleep 0.5
    if curl -s "$BASE/mem?addr=1000&len=1" | grep -q '"reply":"7B"'; then typed=1; break; fi
done
[ "$typed" -eq 1 ] \
    && ok "typed BASIC 'POKE 4096,123' executed (mem[\$1000]=7B)" \
    || ko "keys→BASIC POKE did not take effect"

# ─── Sprint 97: parité debug REST (bridge --control + 7 gaps) ─────────

# 9. breakpoints: create, list, delete
curl -s -X POST --data 'addr=0400' "$BASE/break" | grep -q '"reply":"id=0 addr=0400"' \
    && ok "POST /break creates a breakpoint" || ko "POST /break"
curl -s "$BASE/break" | grep -q 'addr=0400' \
    && ok "GET /break lists it" || ko "GET /break"
curl -s -X DELETE "$BASE/break/0" | grep -q '"ok":true' \
    && ok "DELETE /break/0 removes it" || ko "DELETE /break/{id}"

# 10. conditional breakpoint (compound &&, url-encoded)
curl -s -X POST --data-urlencode 'addr=0500' --data-urlencode 'if=A==5 && X==3' "$BASE/break" \
    | grep -q '"ok":true' && ok "POST /break with if=EXPR (compound)" || ko "POST /break conditional"
curl -s -X DELETE "$BASE/break/0" >/dev/null

# 11. watchpoint with access mode
curl -s -X POST --data 'addr=2000&mode=r' "$BASE/watch" | grep -q 'mode=read' \
    && ok "POST /watch addr= mode=r → read watchpoint" || ko "POST /watch mode"
curl -s "$BASE/watch" | grep -q 'addr=2000' \
    && ok "GET /watch lists it" || ko "GET /watch"
curl -s -X DELETE "$BASE/watch/0" >/dev/null

# 12. set a CPU register (the running CPU may overwrite A within microseconds,
#     so only the command acceptance is asserted here; the deterministic
#     register-effect check lives in test-control-dispatch on a halted core).
curl -s -X POST --data 'reg=a&val=42' "$BASE/set" | grep -q '"ok":true' \
    && ok "POST /set reg=a val=42 accepted" || ko "POST /set reg"

# 13. set a VIA register (gap 6)
curl -s -X POST --data 'via=2&val=FF' "$BASE/set" | grep -q '"reply":"via=2 val=FF"' \
    && ok "POST /set via=2 val=FF" || ko "POST /set via"

# 14. disasm one instruction (bridge)
curl -s "$BASE/disasm?addr=C000&n=1" | grep -q '"ok":true' \
    && ok "GET /disasm?addr=&n=" || ko "GET /disasm"

# 15. hunt cheat-finder: seed then narrow (gap 3)
curl -s -X POST "$BASE/hunt" | grep -q 'candidates=65536' \
    && ok "POST /hunt seeds all cells" || ko "POST /hunt seed"
curl -s -X POST --data 'op=same' "$BASE/hunt" | grep -q '"ok":true' \
    && ok "POST /hunt op=same narrows" || ko "POST /hunt refine"
curl -s -X POST --data 'op=clear' "$BASE/hunt" >/dev/null

# 16. memory ⇄ file region roundtrip within the sandbox (gap 4)
curl -s -X POST --data 'addr=3000&bytes=DE+AD+BE+EF' "$BASE/mem" >/dev/null
curl -s -X POST --data 'path=phos_e2e_region.bin&addr=3000&len=4' "$BASE/save" | grep -q 'wrote=4' \
    && ok "POST /save writes a region to file" || ko "POST /save"
curl -s -X POST --data 'addr=3000&bytes=00+00+00+00' "$BASE/mem" >/dev/null
curl -s -X POST --data 'path=phos_e2e_region.bin&addr=3000' "$BASE/load" | grep -q 'loaded=4' \
    && curl -s "$BASE/mem?addr=3000&len=4" | grep -q '"reply":"DE AD BE EF"' \
    && ok "POST /load restores the region (roundtrip)" || ko "POST /load"
rm -f /tmp/phos_e2e_region.bin

# 17. full save-state / load-state through the protocol (gap 5)
curl -s -X POST --data 'path=phos_e2e_state.ost' "$BASE/state/save" | grep -q '"ok":true' \
    && ok "POST /state/save writes .ost" || ko "POST /state/save"
curl -s -X POST --data 'path=phos_e2e_state.ost' "$BASE/state/load" | grep -qE '"reply":"pc=[0-9A-F]{4}"' \
    && ok "POST /state/load restores the machine" || ko "POST /state/load"
rm -f /tmp/phos_e2e_state.ost

# 18. binary % literal in a parameter (gap 7): %100000000000 == $800
curl -s -X POST --data-urlencode 'addr=%100000000000' "$BASE/break" | grep -q 'addr=0800' \
    && ok "POST /break addr=%bin literal → \$0800" || ko "binary literal"
curl -s -X DELETE "$BASE/break/0" >/dev/null

# 19. bank-aware memory read (Epic 6 / US 2): read under the ROM overlay
curl -s "$BASE/mem?addr=C000&len=1&bank=rom" | grep -q '"ok":true' \
    && ok "GET /mem bank=rom (read BASIC ROM under overlay)" || ko "GET /mem bank=rom"
curl -s "$BASE/mem?addr=C000&len=1&bank=ram" | grep -q '"ok":true' \
    && ok "GET /mem bank=ram (read RAM behind ROM)" || ko "GET /mem bank=ram"
curl -s "$BASE/mem?addr=C000&len=1&bank=bogus" | grep -q '"ok":false' \
    && ok "GET /mem bank=bogus rejected" || ko "GET /mem bad bank not rejected"

# 20. conditional tracing (Epic 6 / US 1): arm a ring trace, save it, stop, off
curl -s -X POST --data-urlencode 'spec=now ring:200' "$BASE/trace" | grep -q '"ok":true' \
    && ok "POST /trace arms a ring trace" || ko "POST /trace"
sleep 0.5
curl -s "$BASE/trace" | grep -q 'count=' \
    && ok "GET /trace reports status" || ko "GET /trace"
curl -s -X POST --data 'path=phos_trace_e2e.log' "$BASE/trace/save" | grep -q '"ok":true' \
    && ok "POST /trace/save writes the ring buffer" || ko "POST /trace/save"
curl -s -X POST "$BASE/trace/stop" | grep -q '"ok":true' \
    && ok "POST /trace/stop" || ko "POST /trace/stop"
curl -s -X DELETE "$BASE/trace" | grep -q '"ok":true' \
    && ok "DELETE /trace (off)" || ko "DELETE /trace"
rm -f /tmp/phos_trace_e2e.log

# 21. access-map region breakpoints (Epic 6 / US 3)
curl -s -X POST --data 'start=2000&end=2010&flags=rw' "$BASE/watch-region" | grep -q 'flagged=' \
    && ok "POST /watch-region flags a region r/w" || ko "POST /watch-region"
curl -s "$BASE/watch-region" | grep -q '2000-2010' \
    && ok "GET /watch-region lists the region" || ko "GET /watch-region"
curl -s -X DELETE "$BASE/watch-region" | grep -q '"ok":true' \
    && ok "DELETE /watch-region clears it" || ko "DELETE /watch-region"

# 22. widened inspection coverage (Epic 6 / US 5)
curl -s "$BASE/peek/video" | grep -q 'vid_mode=' \
    && ok "GET /peek/video" || ko "GET /peek/video"
curl -s "$BASE/peek/kbd" | grep -q 'matrix=' \
    && ok "GET /peek/kbd (keyboard matrix)" || ko "GET /peek/kbd"
curl -s "$BASE/peek/joy" | grep -q 'port_a_mask=' \
    && ok "GET /peek/joy" || ko "GET /peek/joy"
curl -s "$BASE/peek/printer" | grep -q 'type=' \
    && ok "GET /peek/printer" || ko "GET /peek/printer"

# 23. symbol groups (Epic 6 / US 4)
curl -s -X POST --data 'group=5&enabled=off' "$BASE/sym/group" | grep -q 'group=5 enabled=0' \
    && ok "POST /sym/group disables a group" || ko "POST /sym/group off"
curl -s -X POST --data 'group=5&enabled=on' "$BASE/sym/group" | grep -q 'enabled=1' \
    && ok "POST /sym/group re-enables it" || ko "POST /sym/group on"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
