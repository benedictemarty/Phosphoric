# Guide Utilisateur Phosphoric

**Version 1.110.0-alpha** | Emulateur ORIC-1 / Atmos

---

## Table des matieres

1. [Installation](#installation)
2. [Demarrage rapide](#demarrage-rapide)
3. [Chargement de programmes](#chargement-de-programmes)
4. [Clavier](#clavier)
5. [Joystick](#joystick)
6. [Video et affichage](#video-et-affichage)
7. [Audio](#audio)
8. [Imprimante et traceur](#imprimante-et-traceur)
9. [Sauvegarde d'etat](#sauvegarde-detat)
10. [Debogueur interactif](#debogueur-interactif)
11. [Trace CPU et profileur](#trace-cpu-et-profileur)
12. [Debogage avance (GDB, IPC, API HTTP)](#debogage-avance-gdb-ipc-api-http)
13. [Analyse de ROM](#analyse-de-rom)
14. [Serie et modems](#serie-et-modems)
15. [LOCI](#loci)
16. [Enregistrement video et replay](#enregistrement-video-et-replay)
17. [ULA-NG (extensions video)](#ula-ng-extensions-video)
18. [Chromecast](#chromecast)
19. [Mode headless et automation](#mode-headless-et-automation)
20. [WebAssembly (navigateur)](#webassembly-navigateur)
21. [Outils de conversion](#outils-de-conversion)
22. [Reference CLI complete](#reference-cli-complete)
23. [Depannage](#depannage)

---

## Installation

### Dependances

```bash
# Debian / Ubuntu
sudo apt-get install build-essential libsdl2-dev

# Fedora
sudo dnf install gcc SDL2-devel

# Arch Linux
sudo pacman -S base-devel sdl2

# Optionnel : support Chromecast
sudo apt-get install libssl-dev
```

### Compilation

```bash
make SDL2=1                    # Build standard avec SDL2
make                           # Build headless (sans SDL2)
make DEBUG=1 SDL2=1            # Build debug (-g -O0)
make SDL2=1 CAST=1             # Avec support Chromecast
make tools                     # Outils de conversion
sudo make install              # Installation dans /usr/local
```

### Verification

```bash
make tests                     # 908 tests (100% doivent passer)
```

---

## Demarrage rapide

Phosphoric necessite un fichier ROM ORIC pour demarrer. Les ROM ne sont pas distribuees avec l'emulateur pour des raisons de copyright.

```bash
# ORIC-1 avec BASIC 1.0
./oric1-emu -r roms/basic10.rom

# ORIC Atmos avec BASIC 1.1 (auto-detecte)
./oric1-emu -r roms/basic11b.rom

# Forcer le modele
./oric1-emu -r roms/basic10.rom --model oric1
./oric1-emu -r roms/basic11b.rom --model atmos
```

### ROMs supportees

| ROM | Taille | Modele | Detection |
|-----|--------|--------|-----------|
| basic10.rom | 16384 octets | ORIC-1 (BASIC 1.0) | JMP $EA59 |
| basic11b.rom | 16384 octets | Atmos (BASIC 1.1) | JMP $ECCC |
| microdis.rom | variable | Microdisc (overlay) | N/A |

---

## Chargement de programmes

### Cassettes (.TAP)

**Chargement interactif (CLOAD) :**
```bash
./oric1-emu -r basic10.rom -t jeu.tap
```
Au prompt BASIC, tapez `CLOAD""` puis Entree. Le programme se charge depuis la cassette virtuelle.

**Chargement rapide (injection directe) :**
```bash
./oric1-emu -r basic10.rom -t jeu.tap -f
```
Le programme est injecte directement en memoire via le patch ROM, sans delai.
Les programmes multi-blocs (ex: TYRANN.TAP) sont supportes : le premier bloc est injecte,
les blocs suivants sont charges par CLOAD via les patches ROM. Les next-line pointers BASIC
stale sont automatiquement recorrigees apres chaque chargement.

**Chargement au niveau signal (`--tape-signal`) :**
```bash
./oric1-emu -r basic10.rom -t jeu.tap --tape-signal
```
Genere la vraie forme d'onde de la bande sur l'entree VIA CB1, lue par la
routine CLOAD **reelle** de la ROM — comme sur une vraie machine ou sous
Euphoric. A utiliser pour les **chargeurs cassette non standard / proteges**
(turbo-loaders, routines de deprotection, sequencement multi-blocs maison) que
les patches ROM ne savent pas reproduire — par exemple *Soccer Manager*
(KnightSoft, dechiffrement `EOR #$55`), qui echoue en mode patch comme sous
Oricutron. Au prompt, `CLOAD""` est tape automatiquement. Incompatible avec `-f`.

> Le chargement se fait a la **vitesse reelle d'une cassette** (~5 min pour
> 45 Ko en fenetre) : c'est le prix de la fidelite. Pour les jeux standard,
> `-f` reste bien plus rapide.

**Sauvegarde cassette (CSAVE) :**
Quand un programme BASIC execute `CSAVE"nom"`, les donnees sont capturees dans un fichier
`nom.tap` dans le repertoire courant. Si le nom est vide (`CSAVE""`), le fichier sera
`csave_output.tap`.

### Disquettes (.DSK)

Le boot disquette necessite le BASIC ROM et le Microdisc ROM :

```bash
# Un seul lecteur (A:)
./oric1-emu -r basic10.rom --disk-rom microdis.rom -d SEDORIC.DSK

# Plusieurs lecteurs (A: B: C: D:)
./oric1-emu -r basic10.rom --disk-rom microdis.rom \
  -d systeme.dsk --disk1 donnees.dsk --disk2 jeux.dsk --disk3 outils.dsk
```

### Systeme de fichiers hote

Partager un repertoire entre le PC et l'emulateur :
```bash
./oric1-emu -r basic10.rom --hostfs /chemin/vers/dossier
```

---

## Clavier

### Disposition clavier

Par defaut, l'emulateur utilise une disposition QWERTY. Pour passer en AZERTY :

```bash
./oric1-emu -r basic10.rom --keyboard azerty
```

En mode AZERTY, l'emulateur utilise les evenements texte SDL2, donc la saisie fonctionne naturellement quelle que soit la disposition physique du clavier.

### Touches speciales ORIC

| Touche PC | Touche ORIC |
|-----------|-------------|
| Escape | ESC |
| Backspace | DEL |
| Left Ctrl | CTRL |
| Left/Right Shift | SHIFT |
| Return/Enter | RETURN |

### Touches de fonction de l'emulateur

| Touche | Fonction |
|--------|----------|
| F1 | Menu aide |
| F2 | Sauvegarde rapide (quicksave) |
| F3 | Changer l'echelle d'affichage (x1 -> x2 -> x3 -> x4) |
| F4 | Chargement rapide (quickload) |
| F5 | Reset a chaud |
| F7 | Dump memoire (64 Ko RAM dans fichier .bin horodate) |
| F9 | Entrer dans le debogueur |
| F10 | Quitter |
| F11 | Plein ecran |
| F12 | Capture d'ecran |

---

## Joystick

Phosphoric emule l'interface joystick IJK, l'adaptateur le plus courant pour l'ORIC. Le joystick est lu via le Port A du PSG (actif bas).

### Mode clavier

```bash
./oric1-emu -r basic10.rom -j keys
```

| Touche | Direction |
|--------|-----------|
| Fleches haut/bas/gauche/droite | Directions |
| Ctrl droit | Feu 1 |
| Alt droit | Feu 2 |

### Mode manette SDL2

```bash
./oric1-emu -r basic10.rom -j gamepad
```

Utilise la premiere manette SDL2 detectee. Les boutons A, B et X correspondent au feu. Le D-pad et le stick analogique gauche controlent les directions. Le branchement a chaud est supporte.

---

## Video et affichage

### Modes video

L'ORIC possede deux modes d'affichage :
- **Mode texte** : 40 colonnes x 28 lignes, 8 couleurs (attributs ink/paper)
- **Mode HIRES** : 240 x 200 pixels, 6 couleurs avec attributs serie

### Echelle d'affichage

```bash
./oric1-emu -r basic10.rom --scale 2
```

| Echelle | Resolution fenetre |
|---------|--------------------|
| x1 | 240 x 224 |
| x2 | 480 x 448 |
| x3 (defaut) | 720 x 672 |
| x4 | 960 x 896 |

Le rendu utilise le scaling nearest-neighbor (pixel-perfect, sans flou). Appuyer sur **F3** pour changer l'echelle en temps reel. **F11** bascule en plein ecran.

### Captures d'ecran

```bash
# Capture a la fermeture
./oric1-emu -r basic10.rom --screenshot sortie.bmp

# Capture apres N cycles
./oric1-emu -r basic10.rom --screenshot-at 1000000:sortie.ppm

# Dump periodique de frames
./oric1-emu -r basic10.rom --frame-dump /tmp/frames --frame-dump-interval 50
```

Formats supportes : PPM (P6 binaire) et BMP (24 bits non compresse).

---

## Audio

L'ORIC utilise un PSG AY-3-8910 (General Instrument) :
- 3 canaux tonaux independants (periode 12 bits)
- 1 generateur de bruit (periode 5 bits, LFSR 17 bits)
- 16 formes d'enveloppe (attaque, decroissance, maintien, alternance)
- Controle mixer (activation tone/bruit par canal)

La sortie audio se fait via SDL2 a 44100 Hz stereo. Le PSG tourne a 1 MHz, comme le materiel original.

### Commandes BASIC

```basic
REM Jouer un son sur le canal A
SOUND 1,100,15

REM Jouer avec enveloppe
PLAY 0,0,0,0

REM Musique simple
MUSIC 1,4,1,15 : MUSIC 2,4,5,15 : PLAY 1,0,1,0
```

---

## Imprimante et traceur

### Imprimante texte (Centronics)

Capture la sortie LPRINT et LLIST dans un fichier texte :

```bash
./oric1-emu -r basic10.rom -p sortie.txt
```

```basic
REM Dans BASIC :
LPRINT "Bonjour le monde"
LLIST
```

### Traceur MCP-40

Emule le traceur 4 couleurs MCP-40 (Sharp CE-150 / CGP-115) :

```bash
./oric1-emu -r basic10.rom -p traceur.bmp --printer-type mcp40
```

Le traceur utilise un framebuffer 480x400 pixels et exporte en BMP a la fermeture.

**Commandes du traceur** (envoyees via LPRINT) :

| Commande | Description |
|----------|-------------|
| H | Home (retour a l'origine) |
| D x,y | Draw (tracer une ligne jusqu'a x,y) |
| M x,y | Move (deplacer sans tracer) |
| J n | Color (changer de stylo : 0=noir, 1=bleu, 2=vert, 3=rouge) |
| P texte | Print (ecrire du texte a la position courante) |
| I | Init (reinitialiser le traceur) |
| L n | LineType (type de trait : 0=continu, 1-4=pointille) |
| Q n | CharSize (taille des caracteres) |

```basic
REM Exemple : tracer un carre rouge
LPRINT "J3"          : REM Stylo rouge
LPRINT "M0,0"        : REM Aller a l'origine
LPRINT "D100,0"      : REM Tracer vers la droite
LPRINT "D100,100"    : REM Tracer vers le haut
LPRINT "D0,100"      : REM Tracer vers la gauche
LPRINT "D0,0"        : REM Fermer le carre
```

---

## Sauvegarde d'etat

### Sauvegarde et restauration rapide

- **F2** : sauvegarde rapide (`oric1_quicksave.ost`)
- **F4** : chargement rapide (`oric1_quicksave.ost`)

### Via la ligne de commande

```bash
# Sauvegarder a la fermeture
./oric1-emu -r basic10.rom --save-state partie.ost

# Charger au demarrage
./oric1-emu -r basic10.rom --load-state partie.ost
```

### Format .ost

Le format binaire `.ost` (Oric Save sTate) contient :
- Header : magic "OST1", version, taille, CRC32
- 10 sections : CPU, MEM (64 KB), VIA, PSG, VID, KBD, FDC, MDC, TAP, META
- Taille typique : ~65 KB
- Le framebuffer est regenere automatiquement au chargement

---

## Debogueur interactif

### Demarrage

```bash
# Entrer dans le debogueur au lancement
./oric1-emu -r basic10.rom --debug

# Definir un breakpoint initial
./oric1-emu -r basic10.rom --break ED8A
```

Pendant l'emulation, appuyer sur **F9** pour entrer dans le debogueur.

### Commandes

| Commande | Alias | Description |
|----------|-------|-------------|
| `s` | `step` | Executer une instruction |
| `n` | `next` | Executer jusqu'au PC suivant (saute les JSR) |
| `c` | `continue` | Reprendre l'emulation |
| `r` | `regs` | Afficher les registres CPU |
| `d [addr] [n]` | | Desassembler n instructions a addr |
| `m addr [n]` | | Dump memoire de n octets a addr |
| `b addr` | | Ajouter un breakpoint (max 16) |
| `bd n` | | Supprimer le breakpoint #n |
| `w addr` | | Ajouter un watchpoint memoire (max 8) |
| `wd n` | | Supprimer le watchpoint #n |
| `via` | | Afficher les registres VIA 6522 |
| `psg` | | Afficher les registres PSG AY-3-8910 |
| `stack` | | Afficher le contenu de la pile |
| `set reg val` | | Modifier un registre (a, x, y, sp, pc, p) |
| `q` | `quit` | Quitter l'emulateur |
| `h` | `help` | Afficher l'aide |

### Exemples

```
dbg> b C000          # Breakpoint a $C000
dbg> c               # Continuer jusqu'au breakpoint
dbg> r               # Voir les registres
dbg> d C000 10       # Desassembler 10 instructions a $C000
dbg> m 0400 64       # Dump 64 octets a $0400
dbg> w 0300          # Watchpoint sur le VIA (port A)
dbg> set a 42        # A = $42
dbg> s               # Step
```

---

## Trace CPU et profileur

### Trace CPU

Enregistre chaque instruction executee avec le desassemblage et l'etat des registres :

```bash
./oric1-emu -r basic10.rom --trace trace.log

# Limiter a N instructions
./oric1-emu -r basic10.rom --trace trace.log --trace-max 10000
```

**Format de sortie** (une ligne par instruction) :
```
CCCCCCCC  AAAA  XX XX XX  MNEMONIC OPERAND       A=XX X=XX Y=XX SP=XX P=XX
00000000  F42D  4C 59 EA  JMP $EA59              A=00 X=00 Y=00 SP=FD P=24
00000003  EA59  A2 FF     LDX #$FF               A=00 X=00 Y=00 SP=FD P=24
```

### Profileur CPU

Genere un rapport de performance a la fermeture :

```bash
./oric1-emu -r basic10.rom --profile profil.txt --cycles 1000000
```

Le rapport contient :
- Total instructions et cycles, moyenne cycles/instruction
- Top 20 adresses les plus executees (avec % du total)
- Top 20 adresses par consommation de cycles
- Histogramme de frequence des opcodes

---

## Debogage avance (GDB, IPC, API HTTP)

Au-dela du debogueur interactif (F9 / `--debug`), Phosphoric expose plusieurs
canaux de pilotage et d'inspection externes.

### GDB remote (`--gdb`)

Serveur **GDB Remote Serial Protocol** : on attache `gdb`, `lldb` ou un IDE
(VS Code, CLion) au 6502 emule pour poser des breakpoints, single-stepper et
lire/ecrire registres et memoire.

```bash
./oric1-emu -r basic11b.rom --gdb=1234      # attend `target remote :1234`
```

Details, exemples de session et mapping registres : [docs/gdb_remote.md](../gdb_remote.md).

### Controle IPC (`--control`)

Protocole texte ligne a ligne sur stdin/stdout (logs sur stderr) pour piloter
l'emulateur depuis un IDE ou un script : injection clavier, lecture memoire,
save/load state, hot-swap media, evenements. C'est le socle d'OricForge.

```bash
./oric1-emu -r basic11b.rom -n --control
```

Grammaire complete des messages : [docs/control_protocol.md](../control_protocol.md).

### API HTTP/REST (`--http-api`, build `HTTPAPI=1`)

Meme dispatch que `--control`, expose en HTTP/JSON : pilotage clavier, etat,
fichiers cassette/disque en sandbox.

```bash
make HTTPAPI=1
./oric1-emu -r basic11b.rom -n --http-api=8888 --http-api-root ./sandbox
```

`--http-api-bind` (defaut 127.0.0.1), `--http-api-root` (sandbox `/tape`,`/disk`).
Reference : [docs/http-api.md](../http-api.md).

### Debogueur TUI et symboles

- `--tui` — debogueur plein ecran ncurses (build `TUI=1`).
- `--symbols FILE` — charge une table de symboles (`.sym`/`.lab`/`.sym65`) pour
  annoter trace et desassemblage.

---

## Analyse de ROM

Analyser une ROM pour extraire des informations structurelles :

```bash
# Afficher sur stdout
./oric1-emu -r basic10.rom --rom-info

# Ecrire dans un fichier
./oric1-emu -r basic10.rom --rom-info rapport.txt
```

Le rapport contient :
- **Vecteurs materiels** : RESET, NMI, IRQ (adresses de la table des vecteurs)
- **Carte des sous-routines** : toutes les cibles JSR/JMP avec le nombre de references
- **Chaines ASCII** : textes detectes dans la ROM (minimum 4 caracteres)
- **Statistiques d'utilisation** : octets de code vs donnees vs remplissage ($00/$FF)

---

## Serie et modems

Phosphoric emule une **ACIA 6551** (base `$031C` par defaut, `--acia-addr`)
et plusieurs cartes serie/MIDI historiques. Le backend se choisit avec
`--serial TYPE` :

| Backend | Description |
|---|---|
| `loopback` | Boucle locale (TX -> RX), pour tester un protocole |
| `tcp:H:P` | Socket TCP (serveur BBS, terminal distant) |
| `pty` | Pseudo-terminal hote (`/dev/pts/N`) |
| `modem:H:P` | Modem Hayes (commandes AT) vers TCP |
| `com:B,D,P,S,DEV` | Vrai port serie hote (baud,bits,parite,stop,device) |
| `file:IN[:OUT]` | Rejoue/capture des octets bruts dans des fichiers |
| `picowifi[:SSID[:PASS]]` | Modem WiFi PicoWiFiModemUSB (ACIA LOCI $0380) |

Options associees : `--serial-v23` (1200/75, Minitel/Prestel), `--serial-baud`
(timing horloge externe realiste), `--serial-buffer N` (FIFO RX anti-overrun),
`--serial-irq-on-rdrf` (mode WDC 65C51), `--serial-tcp-backpressure`
(contre-pression TCP bornee), `--serial-trace FILE` (trace TX/RX horodatee).

```bash
# BBS via TCP, cadence temps reel (indispensable pour le timing reseau)
./oric1-emu -r basic11b.rom --serial tcp:bbs.example.org:6502 --realtime

# Modem WiFi LOCI
./oric1-emu -r basic11b.rom --loci --serial picowifi:MonWiFi:motdepasse
```

### Cartes dediees

- **Digitelec DTL 2000** — `--dtl2000 TRANSPORT` : carte V23 fidele (PIA 6821 +
  ACIA 6850) a `$03F8` (`--dtl2000-addr`). Voir
  [docs/digitelec-dtl2000/](../digitelec-dtl2000/README.md).
- **Mageco MIDI** — `--mageco TRANSPORT` : interface MIDI (ACIA 6850) a `$03FE`,
  31250 baud (`--mageco-addr`) ; transports `midi:` (ALSA, build `MIDI=1`),
  `smf:song.mid`, `file::out.mid`, etc.
- **ORICON** — `--oricon TRANSPORT` : variante MIDI (MC6850 a `$031C-$031F`,
  compatible LOCI).

Guide serie cote programme ORIC : [docs/orictel-serial-guide.md](../orictel-serial-guide.md),
modem Hayes : [docs/orictel-modem-hayes.md](../orictel-modem-hayes.md).

---

## LOCI

**LOCI** (Lovely Oric Computer Interface, sodiumlb 2024) est une cartouche
RP2040 branchee sur le bus de l'Oric : stockage de masse (USB / SD / flash
interne), clavier-souris-manettes USB HID et modem WiFi. Phosphoric emule sa
MIA (interface memoire) a `$03A0-$03BF` avec `--loci`.

| Option | Role |
|---|---|
| `--loci` | Active la MIA LOCI a `$03A0-$03BF` |
| `--loci-flash DIR` | Racine sandbox pour les operations fichier LOCI |
| `--loci-sdimg PATH` | Image SD FAT16/32 brute (lecture seule) |
| `--loci-usb DIR\|none` | Attache DIR comme cle USB (repetable, 4 max) |
| `--loci-web URL` | Lecteur A servi par un serveur web + autoboot Sedoric natif |
| `--loci-web-base URL` | Pseudo-device « W: Web disks » dans le menu LOCI |
| `--loci-mia-window LO-HI` | Modelise la plage MIA `tior` fiable (0-31) |
| `--loci-irq-latency US` | Cout de transport I2C des IRQ LOCI |

Le **bouton Action** (F8) declenche un snapshot de session puis le menu LOCI
(appui court) ou le diagnostic ROM (appui long). Boote un master Sedoric V4
complet via le firmware LOCI.

```bash
./oric1-emu -r basic11b.rom --loci --loci-flash ./loci_files
```

Documentation complete (menu, ABI firmware, timings, cles USB) :
[docs/loci.md](../loci.md).

---

## Enregistrement video et replay

- **Video AVI** — `--video FILE` enregistre un AVI Motion-JPEG (`--video-fps`,
  defaut 50 ; `--video-quality` 1..100, defaut 85). Le son PSG est muxe (headless
  comme GUI). `--export-border` inclut la bordure d'overscan.
- **Audio WAV** — `--audio-wav FILE` capture le PSG en WAV 16 bits stereo
  44,1 kHz (mode headless).
- **Dump de frames** — `--frame-dump DIR` + `--frame-dump-interval N`.
- **Record / replay deterministe** — `--record FILE` enregistre les entrees
  clavier d'une session, `--replay FILE` les rejoue a l'identique (style « TAS » :
  seule la matrice clavier est non deterministe, elle est capturee par frame).

```bash
# Enregistrer une demo puis la rejouer en video
./oric1-emu -r basic11b.rom --record demo.phm
./oric1-emu -r basic11b.rom --replay demo.phm --video demo.avi
```

Details : [docs/movie_replay.md](../movie_replay.md).

---

## ULA-NG (extensions video)

**ULA-NG** est une ULA « next-generation » : extensions video (palette indirecte,
IRQ raster, scroll fin, sprites materiels, chunky 4bpp, texte 80 colonnes)
activees par un mecanisme de deverrouillage — **indiscernable d'une ULA HCS 10017
standard tant qu'elle reste verrouillee**. Registres a `$0340-$035F`.

`--ula-ng-poke SEQ` programme ces registres au demarrage (`SEQ` = paires
`AAA=VV` hex separees par des virgules) :

```bash
./oric1-emu -r basic11b.rom --ula-ng-poke 340=4E,340=47,341=01,348=07,349=00,34A=F0
```

Specification et guide : [docs/ula-ng/](../ula-ng/README-ULA-NG.md).

---

## Chromecast

### Serveur MJPEG

Diffuse l'ecran de l'emulateur en streaming MJPEG :

```bash
./oric1-emu -r basic10.rom --cast-server
# Serveur demarre sur http://localhost:8080/stream

./oric1-emu -r basic10.rom --cast-server=9090
# Port personnalise
```

Points d'acces :
- `/stream` : flux MJPEG video (720x672, 3x upscale)
- `/audio` : flux WAV audio (PSG en temps reel)

### Cast natif Chromecast (CASTV2)

```bash
# Decouvrir les appareils Chromecast sur le reseau
./oric1-emu -r basic10.rom --cast-discover

# Caster vers un Chromecast
./oric1-emu -r basic10.rom --cast-server --cast-to
./oric1-emu -r basic10.rom --cast-server --cast-to="Salon"
```

Le protocole CASTV2 natif inclut : TLS, protobuf, heartbeat PING/PONG, lancement DashCast.

---

## Mode headless et automation

### Mode headless

Executer sans affichage (pour CI, tests, scripting) :

```bash
./oric1-emu -r basic10.rom --headless --cycles 1000000
```

### Saisie clavier automatique

Simuler des frappes clavier apres un delai en cycles :

```bash
# Taper CLOAD"" + Entree apres 3M cycles
./oric1-emu -r basic10.rom -t jeu.tap --headless \
  --type-keys '3000000:CLOAD""\n'

# Sequences speciales :
#   \n  = touche RETURN
#   \pN = pause de N secondes (1-9)
```

### Sortie verbose

```bash
./oric1-emu -r basic10.rom -v    # Logs DEBUG
```

---

## WebAssembly (navigateur)

Phosphoric compile en **WebAssembly** via Emscripten : l'emulateur complet tourne
dans un onglet, rendu sur un `<canvas>`, audio via Web Audio, clavier via le DOM.

```bash
make wasm     # necessite l'emsdk Emscripten (voir docs/wasm.md)
```

La page (`web/shell.html` + `web/shell.js`) offre un rail d'icones facon JOric
(selecteur ROM, glisser-deposer `.tap`/`.dsk`, Reset, plein ecran, filtre CRT,
save/restore `.ost`), des LEDs d'activite TAPE/DISK et un clavier ORIC fidele en
overlay. **Liens profonds** : `?rom=oric1|atmos` et `?media=<fichier>.tap|.dsk`
demarrent directement sur un programme (un `.dsk` active automatiquement le
Microdisc). Sortie **byte-identique** au build natif.

Guide de deploiement (dont la CSP requise) : [docs/wasm.md](../wasm.md).

---

## Outils de conversion

### bas2tap — BASIC vers cassette

Convertir un fichier texte BASIC en format .TAP :

```bash
./bas2tap programme.bas -o programme.tap
```

Le fichier BASIC doit contenir des lignes numerotees :
```basic
10 PRINT "BONJOUR LE MONDE"
20 GOTO 10
```

### bin2tap — Binaire vers cassette

Convertir un binaire machine en .TAP avec adresse de chargement/execution :

```bash
./bin2tap programme.bin --start 0x0400 --exec 0x0400 -o programme.tap
```

### tap2sedoric — Cassette vers disquette Sedoric

Injecter un fichier .TAP dans une copie d'une disquette Sedoric (MFM_DISK) : il
apparait au catalogue (`DIR`) et peut etre charge/execute.

```bash
# base.dsk = une disquette Sedoric MFM existante (ex. disks/SEDO40u.DSK)
./tap2sedoric programme.tap -o disque.dsk -b base.dsk -n PROG.COM

# fichier machine auto-executable (AUTO) : charge ET s'execute via LOAD"PROG"
./tap2sedoric programme.tap -o disque.dsk -b base.dsk -n PROG.COM -a -e 0x5000

# poser un autoexec de boot (INIST) qui lance le fichier au demarrage
./tap2sedoric programme.tap -o disque.dsk -b base.dsk -n PROG.COM -a -i 'LOAD"PROG"'
```

Options : `-n NOM.EXT` (nom Sedoric), `-a` (AUTO), `-e EXEC_HEX` (adresse
d'execution), `-i "INIST"` (autoexec de boot). Le catalogue est etendu
automatiquement (chainage) au-dela de ~15 fichiers. Format et recette detailles
dans [`docs/SEDORIC.md`](../SEDORIC.md).

### sedoric-info — Inspecter une disquette Sedoric

Affiche la VTOC (secteurs libres / nombre de fichiers), le nom du disque,
l'INIST, le catalogue et les descripteurs decodes (type / load / end / exec) :

```bash
./sedoric-info disque.dsk
./sedoric-info disque.dsk --check 1445:95   # garde de regression sur la VTOC
```

### Chaine RAW et master « nu » (scripts Python)

Alternative operant en RAW (offsets directs), puis conversion en MFM :

```bash
# injecter un binaire (exec fourni => fichier AUTO) puis convertir RAW -> MFM
python3 tools/sedoric_inject.py base.raw prog.bin 0x5000 PROG.COM out.raw 42 17 "" 0x5000
python3 tools/dsk_raw2mfm.py out.raw out.dsk sidemajor 2 42 17

# master Sedoric « nu » : neutralise l'INIST -> boot direct au prompt Ready
python3 tools/sedoric_mkbare.py disks/SEDO40u.DSK bare.dsk
# ... ou remplace l'INIST pour autolancer un programme au boot
python3 tools/sedoric_mkbare.py disks/SEDO40u.DSK auto.dsk 'LOAD"PROG"'
```

### Lancer un programme machine sous Sedoric

Une fois le disque monte et Sedoric demarre (`Ready`), un fichier **`.COM`
AUTO** se lance avec la commande **`LOAD`** de Sedoric :

```
LOAD"PROG"
```

Attention : la commande est bien `LOAD` (et non `LOADM`, qui est la commande
cassette de la ROM et renvoie `?TYPE MISMATCH ERROR`). Taper le nom nu ne lance
qu'un programme BASIC (`?SYNTAX ERROR` pour un binaire). L'extension par defaut
est `.COM` (un `.BIN` donnerait `?FILE NOT FOUND ERROR`).

---

## Reference CLI complete

> Liste exhaustive alignee sur `./oric1-emu --help` (v1.110.0-alpha).
> Certaines options exigent un build specifique : `--tui` (TUI=1),
> `--http-api` (HTTPAPI=1), `--cast-*` (CAST=1), backend `midi:` (MIDI=1).

```
./oric1-emu [OPTIONS]

ROM, modele et hote :
  -r, --rom FILE             Charger un fichier ROM (BASIC 1.0/1.1)
  -m, --model MODEL          Modele : oric1 ou atmos (defaut : auto-detection)
  -k, --keyboard LAYOUT      Disposition clavier : qwerty (defaut) ou azerty
  -h, --hostfs PATH          Monter un repertoire hote (partage de fichiers)

Cassette :
  -t, --tape FILE            Charger un fichier cassette .TAP
  -f, --fast-load            Chargement rapide (injection memoire, sans CLOAD)
      --tape-signal          Cassette au niveau signal (onde VIA CB1, lecture ROM
                             reelle) — loaders custom/proteges ; exclut -f
      --tape-out-capture FILE  Capturer l'onde CSAVE (PB7/Timer1) et la decoder
                             en .TAP (voie A ; desactive les hooks CSAVE)

Disquette (Microdisc WD1793) :
  -d, --disk FILE            Charger une image .DSK dans le lecteur A
      --disk1 FILE           Image .DSK dans le lecteur B
      --disk2 FILE           Image .DSK dans le lecteur C
      --disk3 FILE           Image .DSK dans le lecteur D
      --disk-rom FILE        Charger la ROM Microdisc (microdis.rom)
      --disk-writeback       Reecrire les modifications disque dans les .dsk a la
                             sortie (en place ; seuls les lecteurs ecrits sont sauves)
      --disk-create FILE     Creer une disquette Sedoric vierge (lecteur A) -> FILE
      --disk-web URL         Lecteur A servi par un serveur web (loci-webdisk archi B),
                             pistes MFM lues par HTTP a la demande via le Microdisc
      --fdc-timing MODE      Timing WD1793 : real (defaut, 3" mecanique) ou fast
      --bad-sector [D:]S:T:N Marquer secteur illisible (RNF) : lecteur D (defaut A),
                             face S, piste T, secteur N ; repetable

Sauvegarde d'etat :
      --save-state FILE      Sauvegarder l'etat a la fermeture (.ost)
      --load-state FILE      Charger l'etat au demarrage

Video et affichage :
      --scale N              Echelle : 1, 2, 3 (defaut) ou 4
      --render-software      Forcer le renderer SDL logiciel (corrige fenetre noire
                             sur certains GPU/pilotes)
      --no-border            Desactiver la bordure d'overscan dans la fenetre
      --export-border        Inclure la bordure d'overscan dans les exports image/AVI
      --ula-ng-poke SEQ      Programmer les registres ULA-NG ($0340-$035F) au boot,
                             SEQ = paires AAA=VV hex separees par des virgules

Captures et export :
      --screenshot FILE          Capture a la fermeture (.ppm ou .bmp ; .png supporte)
      --screenshot-at C:FILE     Capture apres C cycles (famille -at REPETABLE)
      --screenshot-when A:V:FILE Capture quand RAM[A]==V (A,V hex ; exit 2 si jamais)
      --screenshot-text FILE     Dump texte ecran ($BB80, 40x28) en ASCII a la fermeture
      --screenshot-text-at C:FILE   Dump texte apres C cycles
      --screenshot-text-when A:V:FILE  Dump texte quand RAM[A]==V (A,V hex)
      --screenshot-ansi FILE     Dump framebuffer en texte ANSI true-color a la fermeture
      --screenshot-ansi-at C:FILE   Dump ANSI apres C cycles
      --dump-ram-at C:FILE       Dump 64 Ko RAM quand cycle >= C
      --dump-ram-when A:V:FILE   Dump 64 Ko quand RAM[A]==V (A,V hex ; exit 2 si jamais)
      --frame-dump DIR           Dump periodique des frames dans un repertoire
      --frame-dump-interval N    Dumper une frame sur N (defaut 50)
      --video FILE               Enregistrer une video Motion-JPEG AVI
      --video-fps N              Cadence d'enregistrement (defaut 50)
      --video-quality N          Qualite JPEG 1..100 (defaut 85)

Audio :
      --audio-wav FILE       Capturer le PSG en WAV 16 bits stereo 44,1 kHz (headless)
      --psg-trace FILE       Journaliser les ecritures registres AY (0-13) + cycle CPU

Headless et automation :
  -n, --headless             Sans affichage (mode headless)
      --realtime             Cadencer a 50 Hz PAL meme en headless (nanosleep) ;
                             requis pour le timing serie reseau et --type-keys deterministe
  -c, --cycles NUM           Executer N cycles puis quitter
  -v, --verbose              Logs verbeux
      --type-keys C:TEXT     Saisie clavier auto apres C cycles (echappements \n \e
                             \b \u \d \l \r \Cx \Fx \Lx \Rx \pN ; repetable)
      --type-keys-when A:V:TEXT  Armer --type-keys quand RAM[A]==V (A,V hex)
      --poke-at C:ADDR=VAL       Ecrire RAM[ADDR]=VAL une fois apres C cycles (hex ; repetable)
      --poke-when A:V:ADDR=VAL   Ecrire RAM[ADDR]=VAL une fois quand RAM[A]==V (hex ; repetable)
      --record FILE          Enregistrer les entrees clavier (replay deterministe)
      --replay FILE          Rejouer un film d'entrees (ignore le clavier live)
      --bench                Bench de debit headless (`BENCH cycles=... mhz_eq=...`)

Peripheriques d'entree/sortie :
  -j, --joystick MODE        Joystick : keys (fleches) ou gamepad (manette SDL2)
  -p, --printer FILE         Capturer la sortie imprimante (LPRINT/LLIST)
      --printer-type TYPE    Type : text (defaut) ou mcp40 (traceur 4 couleurs)

Debogueur, trace et profil :
  -D, --debug                Demarrer dans le debogueur (break a la 1re instruction)
  -b, --breakpoint ADDR      Break quand PC atteint ADDR (hex)
      --break ADDR           Breakpoint initial du debogueur interactif (hex)
      --tui                  Debogueur TUI ncurses (build TUI=1)
      --gdb[=PORT]           Stub GDB distant sur TCP PORT (defaut 1234)
      --control              Mode IPC pour integration IDE (protocole stdin)
      --symbols FILE         Charger une table de symboles (.sym/.lab/.sym65)
      --trace FILE           Trace CPU instruction par instruction
      --trace-max N          Limite d'instructions tracees (garde les N PREMIERES)
      --trace-ring N         Garder les N DERNIERES instructions (ring, ecrit a la sortie)
      --trace-irq FILE       Journaliser chaque entree IRQ + RTI
      --kbd-scan-trace FILE  Journaliser chaque lecture VIA Port B (col, reg7, reg14, matrix, PB3)
      --profile FILE         Ecrire un profil de performance CPU a la fermeture
      --rom-info [FILE]      Analyser la ROM (vecteurs, cibles, chaines)

Serie (ACIA 6551) et modems :
      --serial TYPE          loopback, tcp:H:P, pty, modem:H:P, com:B,D,P,S,DEV,
                             file:IN[:OUT], picowifi[:SSID[:PASS]]
      --serial-v23           Mode V23 : 1200/75 baud (Minitel/Prestel/Digitelec)
      --serial-buffer N      FIFO RX de N octets (anti-overrun ; defaut : off)
      --serial-baud N        Baud horloge externe (timing realiste vs transfert instantane)
      --serial-irq-on-rdrf   Mode IRQ WDC 65C51 (re-declenche tant que RDRF pose)
      --serial-trace FILE    Trace serie (TX/RX/signaux horodates)
      --serial-tcp-backpressure[=N]  Contre-pression bornee pour tcp: (cap SO_RCVBUF)
      --acia-addr ADDR       Adresse de base ACIA en hex (defaut 031C)

Cartes serie/MIDI dediees :
      --dtl2000 TRANSPORT    Digitelec DTL 2000 (PIA 6821 + ACIA 6850) a $03F8
      --dtl2000-addr ADDR    Adresse de base DTL 2000 en hex (defaut 03F8)
      --mageco TRANSPORT     Interface MIDI Mageco (ACIA 6850) a $03FE, 31250 baud
      --mageco-addr ADDR     Adresse de base Mageco en hex (defaut 03FE)
      --oricon TRANSPORT     Variante MIDI ORICON (MC6850 a $031C-$031F, compat LOCI)

LOCI (Lovely Oric Computer Interface) :
      --loci                 Activer la MIA LOCI a $03A0-$03BF
      --loci-flash DIR       Racine sandbox des ops fichier LOCI (implique --loci)
      --loci-sdimg PATH      Image SD FAT16/32 brute (lecture seule ; exclut --loci-flash)
      --loci-usb DIR|none    Attacher DIR comme cle USB LOCI (repetable, 4 max ; 'none' desactive)
      --loci-web URL         Lecteur A LOCI servi par serveur web + autoboot Sedoric natif
      --loci-web-base URL    Pseudo-device « W: Web disks » dans le menu LOCI
      --loci-mia-window LO-HI  Modeliser la plage MIA tior fiable (0-31)
      --loci-irq-latency US  Cout transport I2C des IRQ LOCI (differe chaque /IRQ de US us)

Chromecast :
      --cast-server[=PORT]   Serveur MJPEG (defaut 8080)
      --cast-to[=DEVICE]     Caster vers un Chromecast (CASTV2 natif)
      --cast-discover        Decouvrir les Chromecast sur le reseau

API HTTP (build HTTPAPI=1) :
      --http-api[=PORT]      API de controle HTTP/REST (defaut 8888)
      --http-api-bind ADDR   Adresse d'ecoute de l'API (defaut 127.0.0.1)
      --http-api-root DIR    Racine sandbox des ops fichier /tape,/disk (defaut CWD)

Aide :
  -?, --help                 Afficher l'aide

Touches de fonction (fenetre SDL) :
  F1 Aide  F2 Save rapide  F3 Echelle  F4 Load rapide  F5 Reset
  F6 OSD cassette/disquette a chaud  F8 Bouton Action LOCI  F9 Debogueur
  F10 Quitter  F11 Plein ecran  F12 Capture d'ecran
```

---

## Depannage

**Pas de son** : Verifier que le build utilise `SDL2=1` et que le volume systeme n'est pas coupe.

**Clavier inactif apres boot Sedoric** : Bug corrige en v1.0.0-beta.8. Le timer T1 du VIA se reasserte correctement apres l'initialisation Sedoric.

**Le programme ne se charge pas avec CLOAD** : Verifier que le fichier .TAP est valide. Essayer le mode chargement rapide (`-f`).

**Ecran noir** : Verifier que le fichier ROM est correct (16384 octets). L'emulateur necessite une ROM ORIC valide pour demarrer.

**Performance lente** : L'emulateur tourne a ~90+ MHz equivalent (90x temps reel). Si les performances sont insuffisantes, desactiver le tracage (`--trace`) et le profiling (`--profile`).

**Pas de Chromecast detecte** : Verifier que le PC et le Chromecast sont sur le meme reseau. Le build doit inclure `CAST=1` et les dependances OpenSSL.

---

*Phosphoric v1.110.0-alpha — Guide utilisateur*
*Derniere mise a jour : 2026-08-30*
