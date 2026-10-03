#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
#
# instr_count.sh — instructions executed by the emulator (valgrind/cachegrind),
# a stable performance measure, insensitive to CPU throttling and to code
# layout (unlike the --bench time).
#
# Usage: tools/instr_count.sh BINARY [extra arguments...]
#   e.g.: tools/instr_count.sh ./oric1-emu --mea8000
# Prints the instruction count for 2 M emulated cycles (≈ 100 frames).
# Used in sprint G (cards as modules) to compare two binaries.
#
# Author: bmarty <bmarty@mailo.com>
set -u
B=${1:?usage: instr_count.sh BINAIRE [arguments...]}
shift
command -v valgrind >/dev/null 2>&1 || { echo "instr_count : valgrind absent" >&2; exit 2; }
ROM=roms/basic11b.rom
[ -f "$ROM" ] || ROM=roms/basic10.rom
valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null \
    "$B" -r "$ROM" -n --bench -c "${INSTR_CYCLES:-2000000}" "$@" 2>&1 |
    sed -n 's/.*I *refs: *//p' | tr -d ','
