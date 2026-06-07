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

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Results: $pass passed, $fail failed, $skipped skipped"
echo "═══════════════════════════════════════════════════════"

if [ $fail -gt 0 ]; then
    exit 1
fi
exit 0
