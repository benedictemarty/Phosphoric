#!/usr/bin/env bash
#
# make_bootable_sedoric.sh — fabrique une disquette Sedoric *bootable*, hands-free.
#
# Pilote l'émulateur en headless (--type-keys) pour :
#   1. créer une disquette vierge double face (--disk-create) ;
#   2. démarrer Sedoric depuis une disquette système maître ;
#   3. INIT la disquette vierge (lecteur B) + la rendre « Master disc »
#      (copie du DOS → bootable) ;
#   4. vérifier qu'elle démarre seule (SEDORIC V4.0 / Ready).
#
# Repose sur :
#   - le parseur FDC « Write Track » (v1.25.6),
#   - --disk-create double face (v1.25.7-8).
#
# Usage : tools/make_bootable_sedoric.sh [SORTIE.dsk] [SYSTEME_MAITRE.dsk]
set -euo pipefail

EMU=${EMU:-./oric1-emu}
ROM=${ROM:-roms/basic11b.rom}
DISKROM=${DISKROM:-roms/microdis.rom}
OUT=${1:-bootable.dsk}
MASTER=${2:-disks/SEDO40u.DSK}
NAME=${NAME:-BOOT}

for f in "$EMU" "$ROM" "$DISKROM" "$MASTER"; do
    [ -e "$f" ] || { echo "ERREUR: introuvable: $f" >&2; exit 1; }
done

echo "==> 1) disquette vierge double face -> $OUT"
"$EMU" -r "$ROM" --disk-rom "$DISKROM" --disk-create "$OUT" -n -c 50000 >/dev/null 2>&1

echo "==> 2/3) boot Sedoric, INIT B + Master disc (hands-free)"
# Cadences (cycles) calées sur un format double face (~84 pistes) :
#   3M  : choix langue (1)            6M  : touche -> BASIC
#   9M  : INIT B                      11-12M : Y (lance le format, répété)
#   160M: nom + RET                   165M : init statement vide (RET)
#   176M: Master disc -> Y            179M : Init another -> N
"$EMU" -r "$ROM" --disk-rom "$DISKROM" -d "$MASTER" --disk1 "$OUT" --disk-writeback -n \
    --type-keys 3000000:1 --type-keys 6000000:'\n' --type-keys 9000000:'INIT B\n' \
    --type-keys 11000000:'Y' --type-keys 11500000:'Y' --type-keys 12000000:'Y' \
    --type-keys 160000000:"${NAME}"'\n' --type-keys 165000000:'\n' \
    --type-keys 176000000:'Y' --type-keys 179000000:'N' \
    -c 205000000 >/dev/null 2>&1

echo "==> 4) vérification : boot depuis $OUT seul"
DUMP=$(mktemp)
"$EMU" -r "$ROM" --disk-rom "$DISKROM" -d "$OUT" -n -c 4000000 \
    --dump-ram-at 3990000:"$DUMP" >/dev/null 2>&1
OKLINE=$(python3 - "$DUMP" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read(); base = 0xBB80
for row in range(28):
    line = d[base+row*40: base+row*40+40]
    s = ''.join(chr(b & 0x7f) if 32 <= (b & 0x7f) < 127 else ' ' for b in line)
    if 'SEDORIC' in s:
        print(s.strip()); break
PY
)
rm -f "$DUMP"

if [ -n "$OKLINE" ]; then
    echo "OK ✅  $OUT démarre : « $OKLINE »"
    exit 0
else
    echo "ECHEC ❌  $OUT ne démarre pas (pas de bannière SEDORIC)" >&2
    exit 1
fi
