#!/usr/bin/env bash
# Menu de lancement des démos OCULA de Phosphoric.
# Usage : demos/ocula/menu.sh [--scale N] [--rom CHEMIN] [--accel]
#   --scale N    facteur d'échelle SDL (défaut 2)
#   --rom FILE   ROM à utiliser (défaut roms/basic11b.rom)
#   --accel      utilise le renderer SDL accéléré (défaut : --render-software,
#                qui évite l'écran noir sur certaines configs GPU)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEMO="$ROOT/demos/ocula"
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

# Lance l'émulateur. $1 = fichier .tap, $2 = flag de profil OCULA.
launch() {
    local tap="$1" profile="$2"
    echo
    echo ">>> $tap   ($profile)   — Ctrl+C dans cette fenêtre pour revenir au menu"
    # shellcheck disable=SC2086
    "$EMU" -r "$ROM" $profile -t "$DEMO/$tap" -f $RENDER --scale "$SCALE"
}

while true; do
    cat <<MENU

╔══════════════════════════════════════════════╗
║            Démos OCULA — Phosphoric           ║
╠══════════════════════════════════════════════╣
║  1) Plasma / raster bars  (palette/scanline)  ║
║  2) HIRES étendu 320x200  (palette animée)    ║
║  3) Palette redéfinissable (cyclage trame)    ║
║  4) Texte 80 colonnes                         ║
║  q) Quitter                                   ║
╚══════════════════════════════════════════════╝
MENU
    printf "Choix : "
    read -r choice || break
    case "$choice" in
        1) launch oculaplasma.tap   "--ula ocula" ;;
        2) launch oculahr.tap       "--ula ocula" ;;
        3) launch ocula_demo.tap    "--ula ocula" ;;
        4) launch ocula80_demo.tap  "--ocula-80col-basic" ;;
        q|Q) echo "Au revoir."; break ;;
        *) echo "Choix invalide : $choice" ;;
    esac
done
