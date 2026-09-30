#!/bin/sh
# test_iomenu_cli.sh — peripherals menu (F1), command-line side:
# --menu-screenshot, --config / --no-config and reading of phosphoric.cfg.
#
# Rule checked: a phosphoric.cfg in the current directory is only read in
# graphical mode (not with --headless), unless --config is explicit; --no-config ignores it.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EMU="$ROOT/oric1-emu"
ROM="$ROOT/roms/basic11b.rom"
[ -x "$EMU" ] || { echo "SKIP: $EMU non construit"; exit 0; }
[ -f "$ROM" ] || { echo "SKIP: $ROM absent"; exit 0; }
unset PHOSPHORIC_NO_CONFIG   # this test checks precisely the reading of the file
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass+1)); }
ko() { echo "FAIL: $1"; fail=$((fail+1)); }

# 1. Menu capture: PPM 640 × 640.
"$EMU" -n -r "$ROM" -c 1000 --menu-screenshot menu.ppm >out.log 2>&1
if [ $? -eq 0 ] && [ "$(head -c 15 menu.ppm 2>/dev/null)" = "$(printf 'P6\n640 640\n255')" ] &&
   [ "$(wc -c < menu.ppm)" -eq $((640 * 640 * 3 + 15)) ]; then ok "--menu-screenshot écrit un PPM 640×640"
else ko "--menu-screenshot"; fi

# 2. Explicit --config: read even in headless mode, the command line keeps priority.
printf '# test\nclavier=azerty\njoystick=clavier\ncassette_rapide=oui\n' > essai.cfg
"$EMU" -n -r "$ROM" -c 1000 --config essai.cfg >out.log 2>&1
if grep -q 'Configuration : essai.cfg (3 réglage(s) appliqué(s))' out.log &&
   grep -q 'Keyboard layout: AZERTY' out.log; then ok "--config explicite appliqué en headless"
else ko "--config explicite"; cat out.log; fi
"$EMU" -n -r "$ROM" -c 1000 --config essai.cfg -k qwerty >out.log 2>&1
if grep -q 'Configuration : essai.cfg (2 réglage(s)' out.log && ! grep -q 'AZERTY' out.log; then
    ok "la ligne de commande prime sur le fichier"
else ko "priorité de la ligne de commande"; cat out.log; fi

# 3. --config pointing to a missing file: explicit error, code 1.
"$EMU" -n -r "$ROM" -c 1000 --config absent.cfg >out.log 2>&1
rc=$?
if [ $rc -eq 1 ] && grep -q 'Configuration introuvable : absent.cfg' out.log; then ok "--config absent → code 1"
else ko "--config absent (rc=$rc)"; fi

# 4. phosphoric.cfg in the current directory: ignored in headless mode…
cp essai.cfg phosphoric.cfg
"$EMU" -n -r "$ROM" -c 1000 >out.log 2>&1
if ! grep -q 'Configuration :' out.log; then ok "phosphoric.cfg ignoré en headless"
else ko "phosphoric.cfg lu en headless"; fi

# 5. …read in graphical mode (dummy SDL drivers), except with --no-config or
#    PHOSPHORIC_NO_CONFIG.
if "$EMU" --help 2>&1 | grep -q -- '--render-software'; then
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$EMU" -r "$ROM" -c 1000 >out.log 2>&1
    if grep -q 'Configuration : phosphoric.cfg (3 réglage(s)' out.log; then ok "phosphoric.cfg lu en mode graphique"
    elif grep -qiE 'SDL|video' out.log && ! grep -q 'Configuration' out.log && grep -qi 'error' out.log; then
        echo "SKIP: SDL factice indisponible"
    else ko "phosphoric.cfg non lu en mode graphique"; cat out.log; fi
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$EMU" -r "$ROM" -c 1000 --no-config >out.log 2>&1
    if ! grep -q 'Configuration :' out.log; then ok "--no-config respecté"
    else ko "--no-config ignoré"; fi
    PHOSPHORIC_NO_CONFIG=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$EMU" -r "$ROM" -c 1000 >out.log 2>&1
    if ! grep -q 'Configuration :' out.log; then ok "PHOSPHORIC_NO_CONFIG respecté"
    else ko "PHOSPHORIC_NO_CONFIG ignoré"; fi
fi

echo "Tests: $((pass+fail)), Passed: $pass, Failed: $fail"
[ "$fail" -eq 0 ]
