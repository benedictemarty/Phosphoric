#!/usr/bin/env bash
#
# test_sedoric_inject.sh — regression tests for the Sedoric tooling amended from
# the SCUMM-Oric / scoop-oric knowledge base:
#
#   1. tap2sedoric writes a descriptor conforming to the SEDORIC 3.0 manual
#      (sedna3_0.pdf, verified byte-exact): +3 type flag, +8,+9 exec (LE),
#      +A,+B data-sector count (LE). The old code left type=0 and put the
#      sector count at +9,+10 in big-endian (overlapping the exec address).
#   2. AUTO files carry the exec address; -i writes the INIST boot autoexec.
#   3. Multi-file safety: two successive injections no longer collide
#      (both tap2sedoric and sedoric_inject.py used to reallocate from t21 s1,
#      overwriting the first file's descriptor — see docs/SEDORIC.md).
#   4. The RAW chain (sedoric_inject.py + dsk_raw2mfm.py) round-trips through
#      sedoric-info.
#
# Author: bmarty <bmarty@mailo.com>

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

TMP="$(mktemp -d)"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

echo "=== Sedoric tooling regression test ==="

# --- build the tools we need ---
make -s tap2sedoric sedoric-info >/dev/null 2>&1 || { echo "build failed"; exit 1; }

# --- synthesize a minimal but valid RAW Sedoric base (42 tracks, 2 sides) ---
python3 - "$TMP" <<'PY'
import sys, os
TMP = sys.argv[1]
SECSZ, TRACKS, SECTORS, SIDES = 256, 42, 17, 2
raw = bytearray(SIDES * TRACKS * SECTORS * SECSZ)

def off(track, side, sec):        # side-major
    return ((side * TRACKS + track) * SECTORS + (sec - 1)) * SECSZ

# System sector (t20 s1): disk name + empty INIST
sys_off = off(20, 0, 1)
raw[sys_off + 9:sys_off + 30] = b"TESTDISK 15/07/26    "

# VTOC (t20 s2): free sectors + file count (0)
vt = off(20, 0, 2)
free = TRACKS * SECTORS - 60       # arbitrary plausible free count
raw[vt + 2] = free & 0xFF; raw[vt + 3] = (free >> 8) & 0xFF
raw[vt + 4] = 0; raw[vt + 5] = 0

# Directory (t20 s4): empty (link 0, high-water mark 16)
dr = off(20, 0, 4)
raw[dr + 0] = 0; raw[dr + 1] = 0; raw[dr + 2] = 16

open(os.path.join(TMP, "base.raw"), "wb").write(raw)
print("free=%d" % free)
PY

# --- RAW -> MFM ---
python3 tools/dsk_raw2mfm.py "$TMP/base.raw" "$TMP/base.dsk" sidemajor 2 42 17 >/dev/null 2>&1
[ -s "$TMP/base.dsk" ] && ok "dsk_raw2mfm produced an MFM base" || ko "dsk_raw2mfm failed"
head -c8 "$TMP/base.dsk" | grep -q "MFM_DISK" && ok "MFM_DISK signature present" || ko "no MFM_DISK signature"

# --- craft a .tap (300 bytes, autorun) in the format tap2sedoric expects ---
python3 - "$TMP" <<'PY'
import sys, os
TMP = sys.argv[1]
data = bytes(i & 0xFF for i in range(300))     # -> 2 data sectors
start, end = 0x5000, 0x5000 + len(data) - 1
hdr = bytearray(b"\x16\x16\x16\x24")
hdr += bytes([0, 0, 0x00, 0x80])               # +0,1 res | +2 type | +3 autorun
hdr += bytes([end >> 8, end & 0xFF])           # end BE
hdr += bytes([start >> 8, start & 0xFF])       # start BE
hdr += bytes([0]) + b"SCDATA" + b"\x00"
open(os.path.join(TMP, "scdata.tap"), "wb").write(bytes(hdr) + data)
PY

# --- inject #1 (SCDATA, AUTO from tap) then #2 (PROBE) onto the result ---
./tap2sedoric "$TMP/scdata.tap" -o "$TMP/d1.dsk" -b "$TMP/base.dsk" -n SCDATA.BIN >/dev/null 2>&1 \
    && ok "inject #1 (SCDATA) succeeded" || ko "inject #1 failed"
./tap2sedoric "$TMP/scdata.tap" -o "$TMP/d2.dsk" -b "$TMP/d1.dsk" -n PROBE.BIN -i "PROBE" >/dev/null 2>&1 \
    && ok "inject #2 (PROBE) succeeded" || ko "inject #2 failed"

INFO="$(./sedoric-info "$TMP/d2.dsk" 2>/dev/null)"

# --- descriptor format checks ---
echo "$INFO" | grep -q "type=\$41 AUTO  load=\$5000 end=\$512B exec=\$5000 nsec=2" \
    && ok "SCDATA descriptor byte-exact (type/load/end/exec/nsec)" \
    || { ko "SCDATA descriptor wrong"; echo "$INFO" | grep -i scdata -A1; }

# --- multi-file: the two descriptors must live on different sectors ---
sc="$(echo "$INFO" | grep -A0 'SCDATA' | grep -oE 'desc=t[0-9]+ s[0-9]+')"
pr="$(echo "$INFO" | grep -A0 'PROBE'  | grep -oE 'desc=t[0-9]+ s[0-9]+')"
[ -n "$sc" ] && [ -n "$pr" ] && [ "$sc" != "$pr" ] \
    && ok "no descriptor collision ($sc vs $pr)" \
    || ko "descriptor collision: SCDATA=$sc PROBE=$pr"

# --- INIST autoexec written ---
echo "$INFO" | grep -q 'INIST (autoexec): "PROBE"' \
    && ok "INIST autoexec written" || ko "INIST not written"

# --- VTOC counters advanced by both injections ---
echo "$INFO" | grep -qE 'files=2$|files=2 ' && ok "VTOC file count = 2" \
    || { echo "$INFO" | grep -q 'files=2' && ok "VTOC file count = 2" || ko "VTOC file count wrong"; }

# --- pure RAW chain (sedoric_inject.py) multi-file non-collision ---
python3 - "$TMP" <<'PY'
import sys, os
TMP = sys.argv[1]
open(os.path.join(TMP, "b.bin"), "wb").write(bytes(i & 0xFF for i in range(256)))
PY
python3 tools/sedoric_inject.py "$TMP/base.raw" "$TMP/b.bin" 0x5000 A.BIN "$TMP/r1.raw" 42 17 >/dev/null 2>&1
python3 tools/sedoric_inject.py "$TMP/r1.raw"  "$TMP/b.bin" 0x6000 B.BIN "$TMP/r2.raw" 42 17 >/dev/null 2>&1
python3 tools/dsk_raw2mfm.py "$TMP/r2.raw" "$TMP/r2.dsk" sidemajor 2 42 17 >/dev/null 2>&1
RINFO="$(./sedoric-info "$TMP/r2.dsk" 2>/dev/null)"
ra="$(echo "$RINFO" | grep 'A ' | grep -oE 'desc=t[0-9]+ s[0-9]+' | head -1)"
rb="$(echo "$RINFO" | grep 'B ' | grep -oE 'desc=t[0-9]+ s[0-9]+' | head -1)"
[ -n "$ra" ] && [ -n "$rb" ] && [ "$ra" != "$rb" ] \
    && ok "sedoric_inject.py multi-file non-collision ($ra vs $rb)" \
    || ko "sedoric_inject.py collision: A=$ra B=$rb"

# --- directory sector chaining (overflow the first catalogue sector) ---
# Build a RAW base whose first catalogue sector already holds 14 dummy entries;
# injecting 3 more overflows into a chained catalogue sector.
python3 - "$TMP" <<'PY'
import sys, os
TMP = sys.argv[1]
SECSZ, TRACKS, SECTORS, SIDES = 256, 42, 17, 2
raw = bytearray(SIDES * TRACKS * SECTORS * SECSZ)
def off(t, sd, s): return ((sd * TRACKS + t) * SECTORS + (s - 1)) * SECSZ
vt = off(20, 0, 2); free = TRACKS * SECTORS - 60
raw[vt+2] = free & 0xFF; raw[vt+3] = (free >> 8) & 0xFF
dr = off(20, 0, 4)
for k in range(14):                       # 14 dummy entries -> sector nearly full
    e = 16 + k*16
    raw[dr+e:dr+e+9] = (b"DUMMY%03d " % k)[:9]; raw[dr+e+9:dr+e+12] = b"BIN"
    raw[dr+e+12] = 19; raw[dr+e+13] = 1 + k % 15; raw[dr+e+14] = 1; raw[dr+e+15] = 0x40
raw[dr+2] = (16 + 14*16) & 0xFF
open(os.path.join(TMP, "chain.raw"), "wb").write(raw)
open(os.path.join(TMP, "s.bin"), "wb").write(bytes(256))
PY
cp "$TMP/chain.raw" "$TMP/c0.raw"
for NM in FA FB FC; do
    python3 tools/sedoric_inject.py "$TMP/c0.raw" "$TMP/s.bin" 0x5000 $NM.COM "$TMP/c1.raw" 42 17 >/dev/null 2>&1
    mv "$TMP/c1.raw" "$TMP/c0.raw"
done
python3 tools/dsk_raw2mfm.py "$TMP/c0.raw" "$TMP/chain.dsk" sidemajor 2 42 17 >/dev/null 2>&1
CINFO="$(./sedoric-info "$TMP/chain.dsk" 2>/dev/null)"
present=0
for NM in FA FB FC; do echo "$CINFO" | grep -qE "^  $NM " && present=$((present+1)); done
[ "$present" -eq 3 ] && ok "directory chaining: all 3 files past a full sector are listed" \
    || ko "directory chaining: only $present/3 files found (overflow lost)"
# distinct descriptor sectors (the new catalogue sector must not be overwritten)
ndesc=$(echo "$CINFO" | grep -E "^  F[ABC] " | grep -oE 'desc=t[0-9]+ s[0-9]+' | sort -u | wc -l)
[ "$ndesc" -eq 3 ] && ok "directory chaining: 3 distinct descriptor sectors (no overwrite)" \
    || ko "directory chaining: $ndesc distinct descriptors (catalogue sector overwritten?)"
# the first catalogue sector must now link to a second one (link != 0,0)
link="$(python3 - "$TMP/chain.dsk" <<'PY'
import sys
d = open(sys.argv[1], "rb").read(); TRK_RAW=6400; HDR=256
tracks = d[12] | d[13] << 8; base = HDR + 20*TRK_RAW; td = d[base:base+TRK_RAW]
for i in range(TRK_RAW-4):
    if td[i]==0xA1 and td[i+3]==0xFE and td[i+6]==4:
        for j in range(i+10, i+60):
            if td[j]==0xA1 and td[j+3]==0xFB:
                o=base+j+4; print("%d,%d" % (d[o], d[o+1])); break
        break
PY
)"
[ -n "$link" ] && [ "$link" != "0,0" ] \
    && ok "directory chaining: first sector links to a second ($link)" \
    || ko "directory chaining: no link written ($link)"

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
