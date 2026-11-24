#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# tests/integration/test_dsk2hfe.sh
#
# dsk2hfe tool (.dsk MFM_DISK → HFE v1 magnetic image): structure of the file
# produced (HXCPICFE header, track table, size, side interleaving, 0x4489 sync
# marks), and rejection of invalid images WITHOUT touching an existing output
# file. A truncated image caused an out-of-buffer read before 2.12.4
# (regression: tests/fuzz/regressions/hfe/).
#
# Author: bmarty <bmarty@mailo.com>
set -u
cd "$(dirname "$0")/../.." || exit 1

TOOL=./dsk2hfe
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
ok() { echo "  PASS: $1"; pass=$((pass + 1)); }
ko() { echo "  FAIL: $1"; fail=$((fail + 1)); }

echo "=== dsk2hfe : .dsk MFM_DISK → HFE v1 ==="
[ -x "$TOOL" ] || { echo "  SKIP: $TOOL non construit (make tools)"; exit 0; }

# Synthetic images: 1 side × 3 tracks and 2 sides × 2 tracks, 17 sectors.
python3 -c "import sys; open(sys.argv[1],'wb').write(bytes((i*7)&255 for i in range(3*17*256)))" "$TMP/a.raw"
python3 -c "import sys; open(sys.argv[1],'wb').write(bytes((i*5+1)&255 for i in range(2*2*17*256)))" "$TMP/b.raw"
python3 tools/dsk_raw2mfm.py "$TMP/a.raw" "$TMP/a.dsk" sidemajor 1 3 17 >/dev/null
python3 tools/dsk_raw2mfm.py "$TMP/b.raw" "$TMP/b.dsk" sidemajor 2 2 17 >/dev/null

check() {   # check NAME DSK SIDES TRACKS
    local name=$1 dsk=$2 sides=$3 tracks=$4 hfe="$TMP/$1.hfe"
    if ! "$TOOL" "$dsk" "$hfe" >/dev/null; then ko "$name : conversion refusée"; return; fi
    python3 - "$hfe" "$sides" "$tracks" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
sides, tracks = int(sys.argv[2]), int(sys.argv[3])
errs = []
if d[:8] != b"HXCPICFE": errs.append("signature")
if d[9] != tracks or d[10] != sides: errs.append(f"géométrie {d[10]}x{d[9]}")
if struct.unpack_from("<HH", d, 12) != (250, 300): errs.append("débit / vitesse")
if len(d) != 1024 + tracks * 25600: errs.append(f"taille {len(d)}")
for t in range(tracks):
    blk, ln = struct.unpack_from("<HH", d, 512 + 4 * t)
    if blk != 2 + 50 * t or ln != 25600: errs.append(f"table piste {t}")
# Side 0 = first 256 bytes of each 512-byte block; track 0 is reassembled.
side0 = b"".join(d[1024 + k * 512: 1024 + k * 512 + 256] for k in range(50))
side1 = b"".join(d[1024 + k * 512 + 256: 1024 + k * 512 + 512] for k in range(50))
bits = "".join(format(b, "08b")[::-1] for b in side0)      # LSb first
if "0100010010001001" not in bits: errs.append("marque A1 (0x4489) absente")
if sides == 1 and any(side1): errs.append("face 1 non vide")
if sides == 2 and not any(side1): errs.append("face 1 vide")
if errs: print("; ".join(errs))
sys.exit(1 if errs else 0)
PY
    [ $? -eq 0 ] && ok "$name : en-tête, table, taille, faces, marque A1" || ko "$name"
}
check "1 face, 3 pistes" "$TMP/a.dsk" 1 3
check "2 faces, 2 pistes" "$TMP/b.dsk" 2 2

"$TOOL" "$TMP/a.dsk" "$TMP/again.hfe" >/dev/null
cmp -s "$TMP/1 face, 3 pistes.hfe" "$TMP/again.hfe" && ok "sortie déterministe" || ko "sortie différente d'un lancement à l'autre"

refuse() {  # refuse NAME FILE PATTERN
    echo "garder" > "$TMP/out.hfe"
    msg=$("$TOOL" "$2" "$TMP/out.hfe" 2>&1 >/dev/null); rc=$?
    if [ $rc -eq 1 ] && grep -q "$3" <<<"$msg" && [ "$(cat "$TMP/out.hfe")" = garder ]; then
        ok "$1 : refusée, sortie existante intacte"
    else
        ko "$1 : rc=$rc « $msg »"
    fi
}
refuse "image tronquée" tests/fuzz/regressions/hfe/tronque_2faces_84pistes.dsk "truncated image"
head -c $((256 + 6400)) "$TMP/a.dsk" > "$TMP/court.dsk"
refuse "piste manquante (1 sur 3)" "$TMP/court.dsk" "truncated image"
cp "$TMP/a.raw" "$TMP/plat.dsk"
refuse "image sans en-tête MFM_DISK" "$TMP/plat.dsk" "not an MFM_DISK"
python3 -c "
import struct,sys; d=bytearray(open(sys.argv[1],'rb').read()); struct.pack_into('<I', d, 8, 3); open(sys.argv[2],'wb').write(d)" "$TMP/a.dsk" "$TMP/3faces.dsk"
refuse "3 faces annoncées" "$TMP/3faces.dsk" "implausible geometry"

echo "=== result: $pass passed, $fail failed ==="
[ $fail -eq 0 ]
