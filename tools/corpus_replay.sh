#!/usr/bin/env bash
# tools/corpus_replay.sh — corpus de non-régression sur programmes réels (V2-E7, US7.3).
#
# Rejoue chaque cassette de tapes/ (CLOAD"" tapé au prompt, chemin ROM patché)
# et chaque disquette de disks/ (boot Microdisc) pendant un nombre fixe de cycles, capture l'écran (PPM + texte) et compare son empreinte
# au manifeste versionné tests/corpus/manifest.sha256. Les médias, eux, ne sont
# pas versionnés (droits) : un programme absent est SKIP, pas FAIL.
#
# Ce que ça prouve : à cycle égal, l'image est la même qu'à la baseline — donc
# aucun épic n'a déplacé un accès, une interruption ou un fetch ULA visible.
# Ce que ça ne prouve pas : que l'image est celle du vrai matériel.
#
#   tools/corpus_replay.sh check              # verdict contre le manifeste
#   tools/corpus_replay.sh snapshot           # RE-BASELINE (après un changement voulu)
#   tools/corpus_replay.sh check OUTDIR       # garde les captures pour inspection
#
# Chaque entrée du manifeste : <sha256 du PPM>  <nom>  <cycles>  <type>
# Le nombre de cycles est fixé à la baseline et relu depuis le manifeste : on
# compare toujours au même instant.

set -u
cd "$(dirname "$0")/.." || exit 1

EMU=${EMU:-./oric1-emu}
ROM=roms/basic11b.rom
DISK_ROM=roms/microdis.rom
MANIFEST=tests/corpus/manifest.sha256
MODE=${1:-check}
OUT=${2:-}
TAPE_CYCLES=${CORPUS_TAPE_CYCLES:-14000000}
DISK_CYCLES=${CORPUS_DISK_CYCLES:-15000000}

[ -x "$EMU" ] || { echo "  SKIP: $EMU non construit"; exit 0; }
[ -f "$ROM" ] || { echo "  SKIP: $ROM absent"; exit 0; }

keep=1
if [ -z "$OUT" ]; then OUT=$(mktemp -d); keep=0; fi
mkdir -p "$OUT"
trap '[ "$keep" = 0 ] && rm -rf "$OUT"' EXIT

capture() { # name type media cycles → écrit $OUT/name.ppm et .txt, affiche le sha
    local name="$1" type="$2" media="$3" cycles="$4"
    case "$type" in
        tape) "$EMU" -r "$ROM" -n -t "$media" -c "$cycles" \
                  --type-keys-when 'BC9A:52:CLOAD""\n' \
                  --screenshot "$OUT/$name.ppm" --screenshot-text "$OUT/$name.txt" >/dev/null 2>&1 ;;
        disk) "$EMU" -r "$ROM" --disk-rom "$DISK_ROM" -d "$media" -n -c "$cycles" \
                  --screenshot "$OUT/$name.ppm" --screenshot-text "$OUT/$name.txt" >/dev/null 2>&1 ;;
    esac
    [ -f "$OUT/$name.ppm" ] && sha256sum "$OUT/$name.ppm" | cut -d' ' -f1
}

media_of() { # name type → chemin du média, ou vide
    local name="$1" type="$2" f
    if [ "$type" = tape ]; then
        for f in "tapes/$name".tap "tapes/$name".TAP; do [ -f "$f" ] && { echo "$f"; return; }; done
    else
        for f in "disks/$name".dsk "disks/$name".DSK; do [ -f "$f" ] && { echo "$f"; return; }; done
    fi
}

if [ "$MODE" = snapshot ]; then
    echo "=== Corpus : prise de baseline → $MANIFEST ==="
    : > "$MANIFEST.new"
    for f in tapes/*.tap tapes/*.TAP; do
        [ -f "$f" ] || continue
        name=$(basename "$f"); name=${name%.*}
        sha=$(capture "$name" tape "$f" "$TAPE_CYCLES")
        [ -n "$sha" ] && printf '%s  %s  %s  tape\n' "$sha" "$name" "$TAPE_CYCLES" >> "$MANIFEST.new"
        printf '  %-28s %s\n' "$name" "${sha:-(pas de capture)}"
    done
    if [ -f "$DISK_ROM" ]; then
        for f in disks/*.dsk disks/*.DSK; do
            [ -f "$f" ] || continue
            name=$(basename "$f"); name=${name%.*}
            sha=$(capture "$name" disk "$f" "$DISK_CYCLES")
            [ -n "$sha" ] && printf '%s  %s  %s  disk\n' "$sha" "$name" "$DISK_CYCLES" >> "$MANIFEST.new"
            printf '  %-28s %s\n' "$name" "${sha:-(pas de capture)}"
        done
    fi
    sort -k2 "$MANIFEST.new" > "$MANIFEST"; rm -f "$MANIFEST.new"
    echo "  $(wc -l < "$MANIFEST") programmes dans le manifeste"
    exit 0
fi

echo "=== Corpus de non-régression (V2-E7, US7.3) ==="
[ -f "$MANIFEST" ] || { echo "  SKIP: pas de manifeste ($MANIFEST) — tools/corpus_replay.sh snapshot"; exit 0; }
pass=0; fail=0; skip=0
while read -r sha name cycles type; do
    [ -n "$sha" ] || continue
    media=$(media_of "$name" "$type")
    if [ -z "$media" ] || { [ "$type" = disk ] && [ ! -f "$DISK_ROM" ]; }; then
        skip=$((skip + 1)); continue
    fi
    got=$(capture "$name" "$type" "$media" "$cycles")
    if [ "$got" = "$sha" ]; then
        pass=$((pass + 1)); printf '  PASS: %-28s (%s, %s cycles)\n' "$name" "$type" "$cycles"
    else
        fail=$((fail + 1)); printf '  FAIL: %-28s écran différent de la baseline\n' "$name"
        [ "$keep" = 1 ] && echo "        capture : $OUT/$name.ppm / .txt"
    fi
done < "$MANIFEST"
echo "---"
echo "Tests passed: $pass"
echo "Tests failed: $fail"
[ "$skip" -gt 0 ] && echo "Skipped (média absent): $skip"
[ "$fail" -eq 0 ]
