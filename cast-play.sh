#!/bin/bash
# cast-play.sh — Lance un programme ORIC et le caste automatiquement sur Chromecast
# Utilise le client CASTV2 natif (zero dependance externe).
# Usage: ./cast-play.sh [fichier.dsk|fichier.tap] [options]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EMU="$SCRIPT_DIR/phosphoric"
ROM="$SCRIPT_DIR/roms/basic10.rom"
DISK_ROM="$SCRIPT_DIR/roms/microdis.rom"
CAST_PORT=8080
PROGRAM=""
DISCOVER_ONLY=false
FAST_LOAD=false
NO_CAST=false
CAST_DEVICE=""
KEYBOARD="azerty"

# ═══════════════════════════════════════════════════════
#  Couleurs
# ═══════════════════════════════════════════════════════
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

cleanup() {
    echo ""
    echo -e "${YELLOW}[*] Arret...${NC}"
    if [[ -n "$EMU_PID" ]]; then
        kill "$EMU_PID" 2>/dev/null || true
        wait "$EMU_PID" 2>/dev/null || true
    fi
    echo -e "${GREEN}[+] Termine.${NC}"
}
trap cleanup EXIT INT TERM

EMU_PID=""

usage() {
    echo -e "${BOLD}cast-play.sh${NC} — Lance un programme ORIC sur Chromecast (natif CASTV2)"
    echo ""
    echo -e "Usage: ${CYAN}./cast-play.sh${NC} [options] [fichier.dsk|fichier.tap]"
    echo ""
    echo "Options:"
    echo "  -p, --port PORT      Port du serveur MJPEG (defaut: 8080)"
    echo "  -c, --cast DEVICE    Nom du Chromecast cible (defaut: premier trouve)"
    echo "  -d, --discover       Detecter les Chromecast et quitter"
    echo "  -f, --fast-load      Chargement rapide (TAP uniquement)"
    echo "  -k, --keyboard LAYOUT Layout clavier: azerty (defaut) ou qwerty"
    echo "  -n, --no-cast        Mode local uniquement (pas de cast)"
    echo "  -h, --help           Afficher cette aide"
    echo ""
    echo "Exemples:"
    echo "  ./cast-play.sh                             # BASIC sur le Chromecast"
    echo "  ./cast-play.sh disks/3dfongus.dsk          # Cast auto sur le 1er Chromecast"
    echo "  ./cast-play.sh disks/3dfongus.dsk -c Salon # Cast sur 'Salon'"
    echo "  ./cast-play.sh mon_jeu.tap -f              # Cassette fast-load + cast"
    echo "  ./cast-play.sh disks/3dfongus.dsk -n       # Local seulement, pas de cast"
    echo "  ./cast-play.sh --discover                  # Lister les Chromecast"
    echo ""
}

# ═══════════════════════════════════════════════════════
#  Parsing des arguments
# ═══════════════════════════════════════════════════════
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)
            CAST_PORT="$2"; shift 2 ;;
        -c|--cast)
            CAST_DEVICE="$2"; shift 2 ;;
        -d|--discover)
            DISCOVER_ONLY=true; shift ;;
        -f|--fast-load)
            FAST_LOAD=true; shift ;;
        -k|--keyboard)
            KEYBOARD="$2"; shift 2 ;;
        -n|--no-cast)
            NO_CAST=true; shift ;;
        -h|--help)
            usage; exit 0 ;;
        -*)
            echo -e "${RED}Option inconnue: $1${NC}"; usage; exit 1 ;;
        *)
            PROGRAM="$1"; shift ;;
    esac
done

# ═══════════════════════════════════════════════════════
#  Banniere
# ═══════════════════════════════════════════════════════
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "${BOLD}  ORIC-1 Cast Player (natif CASTV2)${NC}"
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo ""

# ═══════════════════════════════════════════════════════
#  Etape 1 : Compilation
# ═══════════════════════════════════════════════════════
if [[ ! -f "$EMU" ]] || ! strings "$EMU" 2>/dev/null | grep -q "castv2"; then
    echo -e "${YELLOW}[1/3] Compilation avec CAST=1 SDL2=1...${NC}"
    make -C "$SCRIPT_DIR" clean >/dev/null 2>&1 || true
    make -C "$SCRIPT_DIR" CAST=1 SDL2=1 -j"$(nproc)" 2>&1 | tail -1
    echo -e "${GREEN}  OK${NC}"
else
    echo -e "${GREEN}[1/3] Binaire deja compile avec CASTV2${NC}"
fi

# ═══════════════════════════════════════════════════════
#  Mode decouverte seule
# ═══════════════════════════════════════════════════════
if $DISCOVER_ONLY; then
    echo ""
    echo -e "${CYAN}[*] Recherche des Chromecast...${NC}"
    echo ""
    "$EMU" --cast-discover
    exit 0
fi

# ═══════════════════════════════════════════════════════
#  Etape 2 : Verification du programme
# ═══════════════════════════════════════════════════════
if [[ ! -f "$ROM" ]]; then
    echo -e "${RED}[!] ROM BASIC introuvable: $ROM${NC}"
    exit 1
fi

if [[ -z "$PROGRAM" ]]; then
    echo -e "${GREEN}[2/3] Programme:${NC} BASIC (pas de programme, boot ROM)"
else
    # Resoudre le chemin relatif
    if [[ ! -f "$PROGRAM" ]] && [[ -f "$SCRIPT_DIR/$PROGRAM" ]]; then
        PROGRAM="$SCRIPT_DIR/$PROGRAM"
    fi

    if [[ ! -f "$PROGRAM" ]]; then
        echo -e "${RED}[!] Fichier introuvable: $PROGRAM${NC}"
        echo ""
        echo -e "  ${BOLD}Programmes disponibles:${NC}"
        find "$SCRIPT_DIR/disks" -name "*.dsk" -printf "    disks/%f\n" 2>/dev/null || true
        find "$SCRIPT_DIR" -maxdepth 1 -name "*.tap" -printf "    %f\n" 2>/dev/null || true
        find "$SCRIPT_DIR/examples" -name "*.tap" -printf "    examples/%f\n" 2>/dev/null || true
        exit 1
    fi

    EXT="${PROGRAM##*.}"
    EXT="${EXT,,}"
    echo -e "${GREEN}[2/3] Programme:${NC} $(basename "$PROGRAM")"
fi

# ═══════════════════════════════════════════════════════
#  Etape 3 : Lancer l'emulateur avec --cast-to natif
# ═══════════════════════════════════════════════════════
CMD=("$EMU" -r "$ROM" -k "$KEYBOARD")

# Ajouter le cast natif CASTV2 (sauf si --no-cast)
if ! $NO_CAST; then
    if [[ -n "$CAST_DEVICE" ]]; then
        CMD+=(--cast-to="$CAST_DEVICE")
    else
        CMD+=(--cast-to)
    fi
    CMD+=(--cast-server="$CAST_PORT")
fi

if [[ -n "$PROGRAM" ]]; then
    case "$EXT" in
        dsk)
            if [[ ! -f "$DISK_ROM" ]]; then
                echo -e "${RED}[!] ROM Microdisc introuvable: $DISK_ROM${NC}"
                exit 1
            fi
            CMD+=(--disk-rom "$DISK_ROM" -d "$PROGRAM")
            ;;
        tap)
            CMD+=(-t "$PROGRAM")
            if $FAST_LOAD; then
                CMD+=(-f)
            fi
            ;;
        *)
            echo -e "${RED}[!] Format non supporte: .$EXT${NC}"
            exit 1
            ;;
    esac
fi

echo -e "${GREEN}[3/3] Demarrage emulateur + CASTV2 natif...${NC}"

if ! $NO_CAST; then
    echo -e "  Chromecast cible: ${CYAN}${CAST_DEVICE:-premier trouve}${NC}"
fi
echo -e "  Clavier: ${CYAN}${KEYBOARD}${NC}"
echo -e "  Port MJPEG: ${CYAN}${CAST_PORT}${NC}"
echo ""

echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "  ${GREEN}ORIC-1 → Chromecast (CASTV2 natif)${NC}"
echo -e "  Ctrl+C pour arreter"
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo ""

# Lancer l'emulateur (bloquant, Ctrl+C envoie SIGINT → cleanup)
"${CMD[@]}" &
EMU_PID=$!
wait "$EMU_PID" 2>/dev/null || true
EMU_PID=""
