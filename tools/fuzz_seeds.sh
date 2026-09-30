#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
#
# fuzz_seeds.sh — builds the seeds of the fuzzing targets (tests/fuzz/).
#
# Usage: tools/fuzz_seeds.sh DIRECTORY TBIN
#   DIRECTORY/<target>/ receives one or two valid inputs per target; TBIN is the
#   directory of the fuzz_<target> binaries (the .ost seed is written by fuzz_ost).
# No ROM nor third-party media: everything is synthetic, so it runs in CI.
#
# Author: bmarty <bmarty@mailo.com>
set -eu
OUT=$1
TBIN=$2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT/disk" "$OUT/tap" "$OUT/ost" "$OUT/smf" "$OUT/sym" "$OUT/cfg"

python3 - "$OUT" <<'PY'
import os, struct, sys
out = sys.argv[1]

# .tap: sync, header (BASIC then machine code), name, data.
def tap(start, data, autorun=0x80, kind=0x80, name=b"FUZZ"):
    end = start + len(data) - 1
    h = b"\x16\x16\x16\x24" + bytes([0, 0, kind, autorun, end >> 8, end & 0xFF,
                                     start >> 8, start & 0xFF, 0]) + name + b"\x00"
    return h + data
open(os.path.join(out, "tap", "code.tap"), "wb").write(tap(0x5000, bytes(range(64))))
open(os.path.join(out, "tap", "deux_blocs.tap"), "wb").write(
    tap(0x0501, b"\x0a\x05\x0a\x00\xba\x00\x00\x00", 0, 0x00, b"B") + tap(0x6000, b"\x60" * 8))

# .mid: format 1, two tracks (tempo, then Note On / Note Off).
def trk(ev): return b"MTrk" + struct.pack(">I", len(ev)) + ev
hdr = b"MThd" + struct.pack(">IHHH", 6, 1, 2, 96)
t0 = trk(b"\x00\xff\x51\x03\x07\xa1\x20\x00\xff\x2f\x00")
t1 = trk(b"\x00\x90\x3c\x7f\x60\x80\x3c\x00\x00\xff\x2f\x00")
open(os.path.join(out, "smf", "deux_pistes.mid"), "wb").write(hdr + t0 + t1)

# Symbols (--symbols).
open(os.path.join(out, "sym", "rom.sym"), "w").write("$F900 RESET\n$E5BD RDBYTE\nC000 BASIC ; commentaire\n")

# Raw Sedoric image: 1 side, 3 tracks, 17 sectors (converted to MFM afterwards).
open(os.path.join(out, "disk", "brute.raw"), "wb").write(bytes((i * 7) & 0xFF for i in range(3 * 17 * 256)))
PY

python3 "$ROOT/tools/dsk_raw2mfm.py" "$OUT/disk/brute.raw" "$OUT/disk/mfm.dsk" sidemajor 1 3 17 >/dev/null
cp "$ROOT/tests/cli_golden/iomenu.cfg" "$OUT/cfg/iomenu.cfg"
"$TBIN/fuzz_ost" --seed "$OUT/ost/machine.ost"
