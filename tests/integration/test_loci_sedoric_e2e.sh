#!/usr/bin/env bash
# tests/integration/test_loci_sedoric_e2e.sh
#
# E2E regression tests for the LOCI/Sedoric pipe.
# Replays sprints 34b0/b1/b2 scenarios and verifies that LOCI is
# byte-identical to the native Microdisc card on the regions we care
# about (screen text or BASIC program area).
#
# Required external assets (skipped gracefully when missing):
#   roms/basic11b.rom            BASIC 1.1 (Atmos)
#   roms/microdis.rom            Microdisc firmware
#   roms/loci/locirom            LOCI firmware
#   loci_demo.img                FAT16 SD image containing SEDO40u.DSK
#   disks/SEDO40u.DSK            Sedoric V4 master disk dump

set -u
cd "$(dirname "$0")/../.." || exit 1

EMU=./oric1-emu
NATIVE_ROM=roms/basic11b.rom
DISK_ROM=roms/microdis.rom
LOCI_ROM=roms/loci/locirom
DSK=disks/SEDO40u.DSK
SDIMG=loci_demo.img

pass=0
fail=0
skipped=0

skip_if_missing() {
    for f in "$@"; do
        if [ ! -e "$f" ]; then
            echo "  SKIP — missing asset: $f"
            skipped=$((skipped+1))
            return 1
        fi
    done
    return 0
}

# $1 = label, $2 = native bin, $3 = loci bin, $4 = offset (decimal), $5 = length
cmp_region() {
    local label="$1" n="$2" l="$3" off="$4" len="$5"
    python3 - "$n" "$l" "$off" "$len" <<'PY'
import sys
n, l, off, length = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
a = open(n, 'rb').read()[off:off+length]
b = open(l, 'rb').read()[off:off+length]
if a == b:
    sys.exit(0)
diffs = sum(1 for i in range(len(a)) if a[i] != b[i])
print(f"    {diffs}/{length} byte diffs at offset 0x{off:X}", file=sys.stderr)
sys.exit(1)
PY
    if [ $? -eq 0 ]; then
        echo "  [PASS] $label"
        pass=$((pass+1))
    else
        echo "  [FAIL] $label"
        fail=$((fail+1))
    fi
}

echo "═══════════════════════════════════════════════════════"
echo "  LOCI/Sedoric E2E regression tests"
echo "═══════════════════════════════════════════════════════"

if [ ! -x "$EMU" ]; then
    echo "  oric1-emu not built — skipping all (run: make SDL2=1 or make)"
    exit 0
fi

# ── Scenario 34b0 — DOS Version (option 6 + drive A) ───────────────
echo ""
echo "Scenario 34b0 — DOS Version (option 6 + drive A)"
if skip_if_missing "$NATIVE_ROM" "$DISK_ROM" "$LOCI_ROM" "$DSK" "$SDIMG"; then
    NAT_BIN=$(mktemp)
    LOC_BIN=$(mktemp)
    "$EMU" -r "$NATIVE_ROM" --disk-rom "$DISK_ROM" -d "$DSK" \
        --headless -c 60000000 \
        --type-keys '35000000:6\p3a' \
        --dump-ram-at 59000000:"$NAT_BIN" >/dev/null 2>&1
    "$EMU" -r "$LOCI_ROM" --loci --loci-sdimg "$SDIMG" \
        --headless -c 60000000 \
        --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p2\p36\p3a' \
        --dump-ram-at 59000000:"$LOC_BIN" >/dev/null 2>&1
    # Screen text $BB80 length 1120 (40x28)
    cmp_region "34b0 screen text (DOS version)" "$NAT_BIN" "$LOC_BIN" $((0xBB80)) 1120
    rm -f "$NAT_BIN" "$LOC_BIN"
fi

# ── Scenario 34b1 — DIR catalogue ──────────────────────────────────
echo ""
echo "Scenario 34b1 — DIR catalogue"
if skip_if_missing "$NATIVE_ROM" "$DISK_ROM" "$LOCI_ROM" "$DSK" "$SDIMG"; then
    NAT_BIN=$(mktemp)
    LOC_BIN=$(mktemp)
    "$EMU" -r "$NATIVE_ROM" --disk-rom "$DISK_ROM" -d "$DSK" \
        --headless -c 55000000 \
        --type-keys '35000000:\n\p3DIR\n' \
        --dump-ram-at 54000000:"$NAT_BIN" >/dev/null 2>&1
    "$EMU" -r "$LOCI_ROM" --loci --loci-sdimg "$SDIMG" \
        --headless -c 60000000 \
        --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p9\p2\n\p3DIR\n' \
        --dump-ram-at 59000000:"$LOC_BIN" >/dev/null 2>&1
    cmp_region "34b1 screen text (DIR)" "$NAT_BIN" "$LOC_BIN" $((0xBB80)) 1120
    rm -f "$NAT_BIN" "$LOC_BIN"
fi

# ── Scenario 34b2 — SAVE/NEW/LOAD round-trip ───────────────────────
echo ""
echo "Scenario 34b2 — SAVE/NEW/LOAD round-trip"
if skip_if_missing "$NATIVE_ROM" "$DISK_ROM" "$LOCI_ROM" "$DSK" "$SDIMG"; then
    NAT_BIN=$(mktemp)
    LOC_BIN=$(mktemp)
    "$EMU" -r "$NATIVE_ROM" --disk-rom "$DISK_ROM" -d "$DSK" \
        --headless -c 80000000 \
        --type-keys '35000000:\n\p310PRINT"HELLO LOCI"\n\p3SAVE"TEST.BAS"\n\p9\p3NEW\n\p2LOAD"TEST.BAS"\n\p9\p3LIST\n\p3' \
        --dump-ram-at 79000000:"$NAT_BIN" >/dev/null 2>&1
    "$EMU" -r "$LOCI_ROM" --loci --loci-sdimg "$SDIMG" \
        --headless -c 120000000 \
        --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p9\p2\n\p310PRINT"HELLO LOCI"\n\p9\p3SAVE"TEST.BAS"\n\p9\p9\p9NEW\n\p3LOAD"TEST.BAS"\n\p9\p9\p9LIST\n\p9' \
        --dump-ram-at 119000000:"$LOC_BIN" >/dev/null 2>&1
    # BASIC text area $0500 length 0x40 (line link + tokens + chaîne + term)
    cmp_region "34b2 BASIC text \$0500-\$053F" "$NAT_BIN" "$LOC_BIN" $((0x500)) $((0x40))
    rm -f "$NAT_BIN" "$LOC_BIN"
fi

# ── Scenario 34b2b — RUN after LOAD (program execution after round-trip) ──
echo ""
echo "Scenario 34b2b — RUN after LOAD (executes reloaded program)"
if skip_if_missing "$NATIVE_ROM" "$DISK_ROM" "$LOCI_ROM" "$DSK" "$SDIMG"; then
    NAT_BIN=$(mktemp)
    LOC_BIN=$(mktemp)
    "$EMU" -r "$NATIVE_ROM" --disk-rom "$DISK_ROM" -d "$DSK" \
        --headless -c 80000000 \
        --type-keys '35000000:\n\p310PRINT"HELLO LOCI"\n\p3SAVE"TEST.BAS"\n\p9\p3NEW\n\p2LOAD"TEST.BAS"\n\p9\p3RUN\n\p3' \
        --dump-ram-at 79000000:"$NAT_BIN" >/dev/null 2>&1
    "$EMU" -r "$LOCI_ROM" --loci --loci-sdimg "$SDIMG" \
        --headless -c 130000000 \
        --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p9\p2\n\p310PRINT"HELLO LOCI"\n\p9\p3SAVE"TEST.BAS"\n\p9\p9\p9NEW\n\p3LOAD"TEST.BAS"\n\p9\p9\p9RUN\n\p9\p3' \
        --dump-ram-at 129000000:"$LOC_BIN" >/dev/null 2>&1
    cmp_region "34b2b screen text after RUN" "$NAT_BIN" "$LOC_BIN" $((0xBB80)) 1120
    rm -f "$NAT_BIN" "$LOC_BIN"
fi

# ── Scenario 34b5 — 007 TAP via LOCI (game load) ──────────────────
echo ""
echo "Scenario 34b5 — 007 « Dangereusement Vôtre » via LOCI/TAP"
TAP=tapes/007.tap
if skip_if_missing "$NATIVE_ROM" "$LOCI_ROM" "$SDIMG" "$TAP"; then
    NAT_BIN=$(mktemp)
    LOC_BIN=$(mktemp)
    # Native baseline: BASIC 1.1 + fast-load of 007.tap.
    "$EMU" -r "$NATIVE_ROM" -t "$TAP" -f \
        --headless -c 30000000 \
        --dump-ram-at 29000000:"$NAT_BIN" >/dev/null 2>&1
    # LOCI: TUI → 't' tape drive → SPACE picker → SPACE select (007.TAP
    # is the alphabetically-first TAP on loci_demo.img) → ESC = MIA_BOOT
    # → ROM swap BASIC 1.1 → CLOAD"" → auto-run.
    "$EMU" -r "$LOCI_ROM" --loci --loci-sdimg "$SDIMG" \
        --headless -c 60000000 \
        --type-keys '15000000:\p3t\p2 \p2 \p2\e\p9\p1CLOAD""\n\p9\p9\p9' \
        --dump-ram-at 55000000:"$LOC_BIN" >/dev/null 2>&1
    cmp_region "34b5 screen text (007 intro)" "$NAT_BIN" "$LOC_BIN" $((0xBB80)) 1120
    rm -f "$NAT_BIN" "$LOC_BIN"
fi

# ── Scenario 35a — IPC control protocol smoke ──────────────────────
echo ""
echo "Scenario 35a — IPC control protocol (OricForge integration)"
if skip_if_missing "$NATIVE_ROM"; then
    OUT=$(printf "hello\nregs\nread \$F88F 4\npeek via\nbreak \$F893\ncontinue\nquit\n" \
        | timeout 5 "$EMU" -r "$NATIVE_ROM" --control 2>/dev/null)
    expected_lines=(
        "EVT ready"
        "EVT stopped pc=F88F cycles=0 reason=break"
        "OK server=phosphoric/"
        "proto=1 caps=step-out,peek,hello,async-pause"
        "OK A=00 X=00 Y=00 SP=FD P=24 PC=F88F cycles=0"
        "OK A2 FF 9A 58"
        "ora=00 orb=00"
        "OK id=0 addr=F893"
        "EVT stopped pc=F893 cycles=6 reason=break"
    )
    all_ok=true
    for expected in "${expected_lines[@]}"; do
        if ! grep -qF "$expected" <<<"$OUT"; then
            echo "    Missing line: $expected"
            all_ok=false
        fi
    done
    if $all_ok; then
        echo "  [PASS] 35a IPC protocol handshake + peek + step + break + quit"
        pass=$((pass+1))
    else
        echo "  [FAIL] 35a IPC protocol"
        echo "$OUT" | head -20 | sed 's/^/    /'
        fail=$((fail+1))
    fi

    # Async pause : send continue, sleep, send pause, then quit.
    OUT2=$({ printf "continue\n"; sleep 0.3; printf "pause\nregs\nquit\n"; } \
        | timeout 5 "$EMU" -r "$NATIVE_ROM" --control 2>/dev/null)
    if grep -qF "reason=user" <<<"$OUT2" && \
       grep -qF "ERR busy" <<<"$OUT2" 2>/dev/null || \
       grep -qF "reason=user" <<<"$OUT2"; then
        echo "  [PASS] 35a async pause-while-running (reason=user)"
        pass=$((pass+1))
    else
        echo "  [FAIL] 35a async pause"
        echo "$OUT2" | head -10 | sed 's/^/    /'
        fail=$((fail+1))
    fi

    # Sprint 35b — watchpoints + reason=watch
    OUT3=$(printf "watch \$0500\nwrite \$0500 AA\ncontinue\nquit\n" \
        | timeout 3 "$EMU" -r "$NATIVE_ROM" --control 2>/dev/null)
    if grep -qF "reason=watch" <<<"$OUT3"; then
        echo "  [PASS] 35b watchpoint fires reason=watch"
        pass=$((pass+1))
    else
        echo "  [FAIL] 35b watchpoint"
        echo "$OUT3" | head -10 | sed 's/^/    /'
        fail=$((fail+1))
    fi

    # Sprint 35b — raster bp + reason=raster
    OUT4=$(printf "raster 100\ncontinue\nquit\n" \
        | timeout 3 "$EMU" -r "$NATIVE_ROM" --control 2>/dev/null)
    if grep -qF "reason=raster" <<<"$OUT4"; then
        echo "  [PASS] 35b raster bp fires reason=raster"
        pass=$((pass+1))
    else
        echo "  [FAIL] 35b raster bp"
        echo "$OUT4" | head -10 | sed 's/^/    /'
        fail=$((fail+1))
    fi

    # Sprint 35b — EVT halt reason=cycle_limit
    OUT5=$(printf "continue\n" \
        | timeout 3 "$EMU" -r "$NATIVE_ROM" --control -c 1000 2>/dev/null)
    if grep -qF "EVT halt" <<<"$OUT5" && grep -qF "reason=cycle_limit" <<<"$OUT5"; then
        echo "  [PASS] 35b EVT halt reason=cycle_limit"
        pass=$((pass+1))
    else
        echo "  [FAIL] 35b EVT halt"
        echo "$OUT5" | head -10 | sed 's/^/    /'
        fail=$((fail+1))
    fi

    # Sprint 35b — disasm + load-sym
    OUT6=$(printf "disasm \$F88F 3\nquit\n" \
        | timeout 3 "$EMU" -r "$NATIVE_ROM" --control 2>/dev/null)
    if grep -qF 'disasm="LDX #$FF"' <<<"$OUT6"; then
        echo "  [PASS] 35b disasm produces parseable output"
        pass=$((pass+1))
    else
        echo "  [FAIL] 35b disasm"
        echo "$OUT6" | head -10 | sed 's/^/    /'
        fail=$((fail+1))
    fi
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Results: $pass passed, $fail failed, $skipped skipped"
echo "═══════════════════════════════════════════════════════"

if [ $fail -gt 0 ]; then
    exit 1
fi
exit 0
