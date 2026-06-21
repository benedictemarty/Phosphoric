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

## Cible de test confirmée — TELEHACK

`telehack.com:23` est un service telnet public, fiable et 100 % ASCII (idéal
pour l'écran 40 colonnes du Oric). Une fois le terminal lancé :

```
ATDT telehack.com:23
```

Réponse réelle obtenue (vrai modem, WiFi réel) :

```
DIALLING telehack.com:23
CONNECT 9600
Connected to TELEHACK port 112
It is ... in Mountain View, California, USA.
There are N local users. There are M hosts on the network.
May the command line live forever.
Command, one of the following:
  2048  advent  eliza  figlet  starwars  rfc  usenet  ...
Type HELP for a detailed command list.
```

Commandes amusantes à essayer une fois connecté : `starwars`, `eliza`,
`figlet hello`, `advent`, `2048`, `today`, `rfc 1`. `CTRL-C` interrompt une
commande BBS (côté TELEHACK), pas le terminal.

Autres BBS telnet vérifiés joignables : `particlesbbs.dyndns.org:6400`,
`bbs.fozztexx.com:23`, `blackflag.acid.org:23`.

> Note : si un `ATDT` renvoie `NO CARRIER (00:00:00)` immédiatement, c'est une
> résolution DNS qui échoue (nom d'hôte inexistant) — pas un problème de la
> chaîne d'émulation.

## Usenet / NNTP (Eternal-September) — terminal CRLF

NNTP exige des fins de ligne **CRLF**, alors que le terminal telehack envoie du
CR seul. Utilise la variante `modem_nntp.bas` (envoie CR+LF sur RETURN) :

```bash
./oric1-emu -r roms/basic11b.rom --loci \
    --serial com:9600,8,N,1,/dev/ttyACM0 --acia-addr 0380 --serial-buffer 8192 \
    -t tools/modem/modem_nntp.tap -f
```

Puis (le `-` = mode transparent/raw, requis pour NNTP) :
```
ATDT-news.eternal-september.org:119      (compte gratuit requis)
AUTHINFO USER <login>
AUTHINFO PASS <motdepasse>
GROUP comp.sys.oric
HEAD <n> / BODY <n> / ARTICLE <n>
POST                                     (publier ; greeting "posting ok")
```
Compte gratuit : https://www.eternal-september.org/ . Lecture ET publication OK.
NB : la frappe au clavier est cadencée par la main (pas de perte) ; un envoi
programmatique de longue chaîne peut perdre des octets (ACIA en instant transfer).

## Fichiers
- `modem_term.bas` — terminal full-duplex interactif (clavier ↔ modem, CR).
- `modem_nntp.bas` — terminal interactif **CRLF** (pour NNTP/Usenet).
- `modem_probe.bas` — sonde minimale : envoie `ATI` et affiche la réponse
  (auto-run, sans interaction). Test rapide.

## Pilote ACIA (modem_term.bas)
ACIA 6551 à `$0380` : data `$0380`, status `$0381` (TDRE=$10, RDRF=$08),
command `$0382` (`$0B` = DTR on, RX-IRQ off, RTS low), control `$0383` (`$1E`).
Boucle full-duplex : draine le RX vers l'écran, puis `KEY$` → TX.

## Validé empiriquement
`TX: ATI\r` → `RX: Pico WiFi modem … WiFi status: CONNECTED … OK` affiché à
l'écran du Oric, via le vrai modem sur le WiFi réel (RSSI live).
