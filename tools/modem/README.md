# Terminal WiFi LOCI — PicoWiFiModemUSB réel via Phosphoric

Pilote le **vrai** PicoWiFiModemUSB (branché en USB sur l'hôte) depuis le Oric
émulé, exactement comme sur un vrai LOCI : modem exposé en **ACIA 6551 à $0380**,
`--loci` activé. Le backend `com:` relie l'ACIA émulée au périphérique réel.

## Pré-requis
- PicoWiFiModemUSB connecté → `/dev/ttyACM0` (vérifier : `lsusb | grep -i pico`).
- Utilisateur dans le groupe `dialout` (accès au port).

## Lancer le terminal (interactif, fenêtre SDL2)
```bash
make SDL2=1
make tools                                   # bas2tap
./bas2tap tools/modem/modem_term.bas -o tools/modem/modem_term.tap --auto-run

./oric1-emu -r roms/basic11b.rom --loci \
    --serial com:9600,8,N,1,/dev/ttyACM0 --acia-addr 0380 \
    --serial-buffer 1024 \
    -t tools/modem/modem_term.tap -f
```
Le programme s'auto-exécute. Tapez des commandes AT au clavier :

| Commande | Effet |
|---|---|
| `ATI` ⏎ | infos modem (WiFi, IP, RSSI…) |
| `AT` ⏎ | test → `OK` |
| `ATDT host:port` ⏎ | appel telnet sortant (BBS, service…) |
| `ATH` ⏎ | raccrocher |
| `ATE0` / `ATE1` | écho modem off/on (le terminal n'a pas d'écho local) |
| `CTRL-C` | quitter le terminal (BREAK BASIC) |

## Fichiers
- `modem_term.bas` — terminal full-duplex interactif (clavier ↔ modem).
- `modem_probe.bas` — sonde minimale : envoie `ATI` et affiche la réponse
  (auto-run, sans interaction). Test rapide.

## Pilote ACIA (modem_term.bas)
ACIA 6551 à `$0380` : data `$0380`, status `$0381` (TDRE=$10, RDRF=$08),
command `$0382` (`$0B` = DTR on, RX-IRQ off, RTS low), control `$0383` (`$1E`).
Boucle full-duplex : draine le RX vers l'écran, puis `KEY$` → TX.

## Validé empiriquement
`TX: ATI\r` → `RX: Pico WiFi modem … WiFi status: CONNECTED … OK` affiché à
l'écran du Oric, via le vrai modem sur le WiFi réel (RSSI live).
