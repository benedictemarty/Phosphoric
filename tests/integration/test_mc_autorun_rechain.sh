#!/usr/bin/env bash
# tests/integration/test_mc_autorun_rechain.sh — sprint 36c
#
# Regression: machine-code TAPs loaded through the REAL ROM CLOAD path
# (auto-CLOAD, no -f) must NOT be BASIC-rechained, and the ROM autorun
# (JMP ($5F) on 1.0 / equivalent on 1.1) must fire with the program
# bytes intact.
#
# Bug history: the rechain gate read the file-type byte at ZP $66, but
# the ROM 1.0 CLOAD header read (LDX #$09 / STA $5D,X / DEX) stores the
# header REVERSED — the type byte lands at $64 ($66 holds reserved
# byte 1, always $00). Every machine-code load was therefore rechained,
# overwriting program bytes with bogus next-line pointers (Asteroids at
# $0500: 12 corrupted pointers, instant crash back to READY — looked
# like "autorun doesn't work on ORIC-1"). Atmos stores the type at
# $02AE (header read at $E4B9 stores to $02A7,X reversed).

set -u
cd "$(dirname "$0")/../.." || exit 1

EMU=./oric1-emu
BIN2TAP=./bin2tap
BAS2TAP=./bas2tap

pass=0
fail=0

if [ ! -x "$EMU" ]; then
    echo "ERROR: $EMU not built" >&2
    exit 1
fi
if [ ! -x "$BIN2TAP" ] || [ ! -x "$BAS2TAP" ]; then
    echo "ERROR: bin2tap/bas2tap not built (make tools)" >&2
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ── Tiny autorun machine-code program @ $0500 ──────────────────────
# 0500: A9 52     LDA #'R'
# 0502: 8D 80 BB  STA $BB80      ; screen RAM, row 0 col 0
# 0505: A9 55     LDA #'U'
# 0507: 8D 81 BB  STA $BB81
# 050A: A9 4E     LDA #'N'
# 050C: 8D 82 BB  STA $BB82
# 050F: 4C 0F 05  JMP $050F      ; spin forever
printf '\xA9\x52\x8D\x80\xBB\xA9\x55\x8D\x81\xBB\xA9\x4E\x8D\x82\xBB\x4C\x0F\x05' \
    > "$WORK/marker.bin"
# NB: addresses MUST carry the 0x prefix — bin2tap uses strtol base 0,
# so a bare "0500" parses as octal ($0140).
"$BIN2TAP" "$WORK/marker.bin" --start 0x0500 --exec 0x0500 \
    -o "$WORK/marker.tap" --name MARKER >/dev/null 2>&1

CYCLES=12000000
DUMP_AT=11000000

# check_mc <label> <rom>
# Runs the autorun MC tap through the real CLOAD path and asserts:
#   1. no "BASIC rechain" in the log (gate honours the type byte)
#   2. "RUN" marker present in screen RAM (autorun fired)
#   3. program bytes at $0500 intact (no pointer corruption)
check_mc() {
    local label="$1" rom="$2"
    if [ ! -f "$rom" ]; then
        echo "  SKIP  ${label} (missing $rom)"
        return
    fi
    local dump="$WORK/${label}.ram" log="$WORK/${label}.log"
    timeout 25 "$EMU" -r "$rom" -t "$WORK/marker.tap" --headless \
        -c "$CYCLES" --dump-ram-at "${DUMP_AT}:${dump}" >"$log" 2>&1
    if grep -q "BASIC rechain" "$log"; then
        echo "  FAIL  ${label} (machine-code load was rechained)"
        fail=$((fail+1))
        return
    fi
    if [ ! -s "$dump" ]; then
        echo "  FAIL  ${label} (no RAM dump)"
        fail=$((fail+1))
        return
    fi
    python3 - "$dump" "$WORK/marker.bin" <<'PYEOF'
import sys
ram = open(sys.argv[1], 'rb').read()
prog = open(sys.argv[2], 'rb').read()
ok = True
if ram[0xBB80:0xBB83] != b'RUN':
    print("    marker absent from screen RAM (autorun did not fire)")
    ok = False
if ram[0x0500:0x0500 + len(prog)] != prog:
    print("    program bytes at $0500 corrupted")
    ok = False
sys.exit(0 if ok else 1)
PYEOF
    if [ $? -eq 0 ]; then
        echo "  PASS  ${label}"
        pass=$((pass+1))
    else
        echo "  FAIL  ${label}"
        fail=$((fail+1))
    fi
}

# check_basic_still_rechains <label> <rom>
# Control case: BASIC programs (type $00) must STILL be rechained
# (guards the TYRANN stale-pointer fix). The next-line pointer of the
# first BASIC line is deliberately corrupted in the TAP so the rechain
# has something to fix (it only logs when fixes > 0).
check_basic_still_rechains() {
    local label="$1" rom="$2"
    if [ ! -f "$rom" ]; then
        echo "  SKIP  ${label} (missing $rom)"
        return
    fi
    printf '10 PRINT "HI"\n20 GOTO 10\n' > "$WORK/prog.bas"
    "$BAS2TAP" "$WORK/prog.bas" -o "$WORK/prog.tap" >/dev/null 2>&1
    python3 - "$WORK/prog.tap" <<'PYEOF'
import sys
path = sys.argv[1]
tap = bytearray(open(path, 'rb').read())
i = tap.index(0x24) + 1 + 9          # skip sync + $24 + 9 header bytes
while tap[i] != 0:                   # skip filename
    i += 1
i += 1
tap[i:i+2] = b'\xFF\xFF'             # stale next-line pointer of line 10
open(path, 'wb').write(tap)
PYEOF
    local log="$WORK/${label}.log"
    timeout 25 "$EMU" -r "$rom" -t "$WORK/prog.tap" --headless \
        -c 9000000 >"$log" 2>&1
    if grep -q "BASIC rechain" "$log"; then
        echo "  PASS  ${label}"
        pass=$((pass+1))
    else
        echo "  FAIL  ${label} (BASIC load no longer rechained)"
        fail=$((fail+1))
    fi
}

echo "MC autorun / rechain-gate regression:"
check_mc "mc-autorun-oric1" "roms/basic10.rom"
check_mc "mc-autorun-atmos" "roms/basic11b.rom"
check_basic_still_rechains "basic-rechain-oric1" "roms/basic10.rom"
check_basic_still_rechains "basic-rechain-atmos" "roms/basic11b.rom"

echo "Result: ${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ]
