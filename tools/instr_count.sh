#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
#
# instr_count.sh — instructions exécutées par l'émulateur (valgrind/cachegrind),
# mesure stable de la performance, insensible au bridage du processeur et à la
# disposition du code (contrairement au temps de --bench).
#
# Usage : tools/instr_count.sh BINAIRE [arguments en plus...]
#   ex. : tools/instr_count.sh ./oric1-emu --mea8000
# Affiche le nombre d'instructions pour 2 M cycles émulés (≈ 100 trames).
# Utilisé au sprint G (cartes en modules) pour comparer deux binaires.
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
