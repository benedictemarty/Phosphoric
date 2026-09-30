#!/usr/bin/env bash
# Launch menu for the Phosphoric ULA-NG demos.
# Usage: demos/ula-ng/menu.sh [--scale N] [--rom PATH] [--accel]
#   --scale N    SDL scale factor (default 2)
#   --rom FILE   ROM to use (default roms/basic11b.rom)
#   --accel      use the accelerated SDL renderer (default: --render-software,
#                which avoids the black screen on some GPU setups)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEMO="$ROOT/demos/ula-ng"
EMU="$ROOT/oric1-emu"

SCALE=2
ROM="$ROOT/roms/basic11b.rom"
RENDER="--render-software"

while [ $# -gt 0 ]; do
    case "$1" in
        --scale) SCALE="$2"; shift 2 ;;
        --rom)   ROM="$2";   shift 2 ;;
        --accel) RENDER="";  shift ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Option inconnue : $1"; exit 1 ;;
    esac
done

if [ ! -x "$EMU" ]; then
    echo "Émulateur introuvable : $EMU"
    echo "Compile-le d'abord :  make SDL2=1"
    exit 1
fi
if [ ! -f "$ROM" ]; then
    echo "ROM introuvable : $ROM   (utilise --rom CHEMIN)"
    exit 1
fi

# Launches the emulator. $1 = .tap file. The ULA-NG unlocks itself.
launch() {
    local tap="$1"
    echo
    echo ">>> $tap   — Ctrl+C dans cette fenêtre pour revenir au menu"
    # shellcheck disable=SC2086
    "$EMU" -r "$ROM" -t "$DEMO/$tap" -f $RENDER --scale "$SCALE"
}

while true; do
    cat <<'MENU'

╔══════════════════════════════════════════════╗
║           Démos ULA-NG — Phosphoric           ║
╠══════════════════════════════════════════════╣
║  1) Chunky 4bpp 320x200 16 couleurs  (§5.8)   ║
║  2) Texte 80 colonnes                (§5.8)   ║
║  3) Attributs : mosaïque par cellule  (§5.6)  ║
║  4) Copper / raster bars arc-en-ciel  (§5.4)  ║
║  5) Sprite matériel 16x16 rebondissant (§5.7) ║
║  6) VDU intégré : mosaïque via flux cmd (NG_VDU)║
║  7) VDU graphique : sunburst (CLG/DRAW, VRAM) ║
║  8) VDU upload : sprite défini par flux (v0.3)║
║  q) Quitter                                   ║
╚══════════════════════════════════════════════╝
   (mode 80 col : patiente ~30 s, remplissage BASIC)
MENU
    printf "Choix : "
    read -r choice || break
    case "$choice" in
        1) launch ng_chunky.tap ;;
        2) launch ng_text80.tap ;;
        3) launch ng_attributes.tap ;;
        4) launch ng_copper.tap ;;
        5) launch ng_sprite.tap ;;
        6) launch ng_vdu.tap ;;
        7) launch ng_vdu_gfx.tap ;;
        8) launch ng_vdu_spr.tap ;;
        q|Q) echo "Au revoir."; break ;;
        *) echo "Choix invalide : $choice" ;;
    esac
done
