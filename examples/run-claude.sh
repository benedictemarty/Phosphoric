#!/usr/bin/env bash
# Lance "Claude sur Oric-1" : l'Oric dialogue avec l'API Anthropic via le VRAI
# modem PicoWiFiModemUSB (USB CDC) relie a l'ACIA LOCI $0380.
#
# Prerequis :
#   1. Le Pico WiFi reel branche (cafe:4001) -> /dev/ttyACM0
#   2. Le Pico connecte a votre WiFi (AT$SSID=.../AT$PASS=.../ATC1, ou wificonf.bas)
#   3. Votre cle API Anthropic dans examples/claude.bas ligne 40 (KY$="sk-ant-...")
#      puis : ./bas2tap examples/claude.bas -o examples/claude.tap --auto-run
#
# Usage : examples/run-claude.sh [/dev/ttyXXX]
set -euo pipefail
cd "$(dirname "$0")/.."

DEV="${1:-/dev/ttyACM0}"
ROM="roms/basic11b.rom"          # Atmos (BASIC 1.1)
TAP="examples/claude.tap"

[ -e "$DEV" ]  || { echo "ERREUR: $DEV absent (Pico branche ?)"; exit 1; }
[ -f "$TAP" ]  || ./bas2tap examples/claude.bas -o "$TAP" --auto-run

# USB CDC : le debit est cosmetique, mais on fixe un etat propre.
stty -F "$DEV" 9600 raw -echo 2>/dev/null || true

exec ./oric1-emu -r "$ROM" \
  --loci --acia-addr 0380 \
  --serial "com:9600,8,N,1,$DEV" \
  --serial-buffer 4096 \
  -t "$TAP" -f
