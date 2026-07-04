# Guide Utilisateur Phosphoric

**Version 1.14.3-alpha** | Emulateur ORIC-1 / Atmos

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
12. [Analyse de ROM](#analyse-de-rom)
13. [Chromecast](#chromecast)
14. [Mode headless et automation](#mode-headless-et-automation)
15. [Outils de conversion](#outils-de-conversion)
16. [Reference CLI complete](#reference-cli-complete)
17. [Depannage](#depannage)

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
make tests                     # 256 tests (100% doivent passer)
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

### tap2sedoric — Cassette vers disquette

Convertir un fichier .TAP en format disquette Sedoric :

```bash
./tap2sedoric programme.tap -o disque.dsk
```

---

## Reference CLI complete

```
./oric1-emu [OPTIONS]

ROM et modele :
  -r, --rom FILE             Charger la ROM BASIC (obligatoire)
  -m, --model MODEL          Forcer le modele : oric1, atmos, 1.0, 1.1

Cassette et disquette :
  -t, --tape FILE            Charger un fichier cassette .TAP
  -f, --fast-load            Chargement rapide (injection memoire)
  -d, --disk FILE            Charger une image .DSK (lecteur A)
      --disk-rom FILE        Charger la ROM Microdisc
      --disk1 FILE           Lecteur B
      --disk2 FILE           Lecteur C
      --disk3 FILE           Lecteur D

Sauvegarde d'etat :
      --save-state FILE      Sauvegarder l'etat a la fermeture
      --load-state FILE      Charger l'etat au demarrage

Joystick :
  -j, --joystick MODE        Mode joystick : keys, gamepad

Imprimante :
  -p, --printer FILE         Capturer la sortie imprimante
      --printer-type TYPE    Type : text (defaut), mcp40

Affichage :
      --scale N              Echelle : 1, 2, 3 (defaut), 4

Trace et profiling :
      --trace FILE           Trace CPU instruction par instruction
      --trace-max N          Limite du nombre d'instructions tracees
      --profile FILE         Rapport de performance CPU a la fermeture

Analyse :
      --rom-info [FILE]      Analyser la ROM (vecteurs, cibles, chaines)

Debogueur :
  -D, --debug                Demarrer dans le debogueur
  -b, --breakpoint ADDR      Breakpoint legacy (hex)
      --break ADDR           Breakpoint debogueur interactif (hex)

Chromecast :
      --cast-server[=PORT]   Demarrer le serveur MJPEG (defaut 8080)
      --cast-to[=DEVICE]     Caster vers un Chromecast
      --cast-discover        Decouvrir les Chromecast sur le reseau

Affichage et export :
  -k, --keyboard LAYOUT      qwerty (defaut) ou azerty
  -n, --headless              Sans affichage
  -c, --cycles N              Executer N cycles puis quitter
      --screenshot FILE       Capture a la fermeture (.ppm/.bmp)
      --screenshot-at N:FILE  Capture apres N cycles
      --frame-dump DIR        Dump periodique des frames
      --frame-dump-interval N Intervalle de dump (defaut 50)
      --type-keys N:TEXT      Saisie clavier automatique
  -h, --hostfs PATH           Monter un repertoire hote
  -v, --verbose               Logs de debogage
  -?, --help                  Afficher l'aide
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

*Phosphoric v1.14.3-alpha — Guide utilisateur*
*Derniere mise a jour : 2026-03-16*
