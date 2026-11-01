# Liste de Compatibilite — Phosphoric v1.110.0-alpha

Derniere mise a jour : 2026-11-01 (balayage de 1318 cassettes, corrige en 2.31.10)

---

## Systemes d'exploitation

| Programme | Format | Modele | Statut | Notes |
|-----------|--------|--------|--------|-------|
| BASIC 1.0 ROM | .ROM | ORIC-1 | Fonctionnel | Interpreteur BASIC complet |
| BASIC 1.1 ROM | .ROM | Atmos | Fonctionnel | Auto-detection via JMP $ECCC |
| Sedoric V4.0 | .DSK | ORIC-1/Atmos | Fonctionnel | Boot, clavier, commandes DOS |
| Sedoric V3.0 | .DSK | ORIC-1/Atmos | Fonctionnel | Boot et commandes DOS |

---

## Jeux cassette (.TAP) — 26/26 fonctionnels

| Programme | Format | Taille | Statut | Notes |
|-----------|--------|--------|--------|-------|
| 007 | .TAP | 28 KB | Fonctionnel | Fast load $0180-$7148 |
| Acheron's Rage | .TAP | 43 KB | Fonctionnel | Fast load |
| Aigle d'Or | .TAP | 54 KB | Fonctionnel | Fast load |
| Andromeda | .TAP | 35 KB | Fonctionnel | Fast load |
| Airline | .TAP | 24 KB | Fonctionnel | Fast load |
| Atlantis | .TAP | 46 KB | Fonctionnel | Chargement OK |
| Author | .TAP | 10 KB | Fonctionnel | Chargement OK |
| Bat Fly | .TAP | 20 KB | Fonctionnel | Chargement OK |
| Bataille Navale | .TAP | 6 KB | Fonctionnel | Fast load |
| Breakout | .TAP | 4 KB | Fonctionnel | Fast load $15BB-$2637, BASIC |
| Bricky | .TAP | 11 KB | Fonctionnel | Fast load |
| Centipede | .TAP | 5 KB | Fonctionnel | Chargement OK |
| Chopper | .TAP | 40 KB | Fonctionnel | Fast load (SCREEN mode) |
| Citadel | .TAP | 49 KB | Fonctionnel | 32 KB charges |
| Cite | .TAP | 47 KB | Fonctionnel | Chargement OK |
| Defender | .TAP | 58 KB | Fonctionnel | Fast load $69FF-$80C7 |
| Johnny | .TAP | 17 KB | Fonctionnel | Chargement OK |
| Loi du West | .TAP | 47 KB | Fonctionnel | Chargement OK |
| Manic Miner | .TAP | 39 KB | Fonctionnel | Chargement OK |
| Manic Miner (proper) | .TAP | 39 KB | Fonctionnel | 24 KB charges |
| Pasta Blasta | .TAP | 22 KB | Fonctionnel | Fast load |
| Psy | .TAP | 33 KB | Fonctionnel | Fast load |
| Poker (poker-asn.tap) | .TAP | — | Fonctionnel | Graphismes HIRES corrects |
| Soccer Manager | .TAP | 44 KB | Fonctionnel | **Chargeur maison + dechiffrement EOR #$55 → `--tape-signal`** (echoue en fast load, comme Oricutron) |
| Spooky | .TAP | 24 KB | Fonctionnel | Chargement OK |
| Spooky (cracked) | .TAP | 24 KB | Fonctionnel | Fast load |

---

## Jeux disque (.DSK) — 11/11 fonctionnels

| Programme | Format | Taille | Geometrie | Statut | Notes |
|-----------|--------|--------|-----------|--------|-------|
| 3D Fongus | .DSK | 1 MB | — | Fonctionnel | Boot Sedoric |
| Aigle d'Or | .DSK | 537 KB | 2 faces x 42 pistes | Fonctionnel | Boot Sedoric |
| Citadelle | .DSK | 141 KB | 1 face x 22 pistes | Fonctionnel | Boot Sedoric |
| Le Manoir du Dr Genius | .DSK | 141 KB | 1 face x 22 pistes | Fonctionnel | Boot Sedoric |
| Manic Miner (Telestrat EN) | .DSK | 537 KB | — | Fonctionnel | Boot Sedoric |
| Manic Miner (Telestrat FR) | .DSK | 537 KB | — | Fonctionnel | Boot Sedoric |
| Oric Chess | .DSK | 141 KB | 1 face x 22 pistes | Fonctionnel | Boot Sedoric |
| Manic Miner | .DSK | 269 KB | — | Fonctionnel | Boot Sedoric |
| Pasta Blasta | .DSK | 1.2 MB | — | Fonctionnel | Boot Sedoric |
| Sedoric V4.0 | .DSK | 1 MB | 2 faces x 80 pistes | Fonctionnel | Systeme Sedoric |
| Sedoric V3.0 | .DSK | 1 MB | — | Fonctionnel | Systeme Sedoric |

---

## Demos / Programmes de test

| Programme | Format | Statut | Notes |
|-----------|--------|--------|-------|
| Hello World | .TAP | Fonctionnel | Mode texte, sortie PRINT |
| Demo sonore | .TAP | Fonctionnel | Generation PSG tone/enveloppe |
| Demo graphique HIRES | .TAP | Fonctionnel | Rendu 240x200 |
| Explode | .TAP | Fonctionnel | CLOAD, gameplay fonctionnel |

---

## Taux de compatibilite

| Categorie | Testes | Fonctionnels | Taux |
|-----------|--------|-------------|------|
| Cassettes (.TAP) | 25 | 25 | **100%** |
| Disques (.DSK) | 11 | 11 | **100%** |
| Demos / tests | 4 | 4 | **100%** |
| **Total** | **40** | **40** | **100%** |

---

## Balayage d'une archive de 1318 cassettes (2.31.8, corrige en 2.31.10)

`tools/tap_sweep.sh DIR [JOBS]` charge chaque `.tap` d'un dossier par le
chemin normal (`CLOAD""` automatique, sans `-f`), 25 M cycles sur la ROM Atmos,
puis relance sur la ROM ORIC-1 les cas suspects. Lance sur
`~/oriclib/games/tap` (archive locale, non versionnee) le 2026-11-01.

**Correction (2.31.10)** : le bilan publie en 2.31.8 annoncait « 0 arret du
6502 ». C'etait faux : l'outil lisait les journaux de l'emulateur sur stderr,
alors qu'ils sortent sur stdout, et ne voyait donc aucun arret. Bilan refait
avec l'outil corrige :

| Atmos / ORIC-1 | Nombre | Lecture |
|----------------|--------|---------|
| OK | 1053 | ne prouve pas que le programme est juste, seulement qu'il ne plante pas |
| `Ready` / `Ready` | 160 | surtout des cassettes sans drapeau d'auto-run (comportement normal) ; 2 lecteurs de musique sous interruption (`SPACEMU2`, `POSDEMO`) |
| `Ready` / OK | 2 | programmes ORIC-1 |
| arret / OK | 74 | programmes ORIC-1 qui plantent sur la ROM Atmos, et marchent sur la ROM 1.0 |
| arret / arret | 16 | 13 cassettes `os*` (lecteurs de musique) et `vache.TAP`, `os2kata`, `os2vwtit` : voir ci-dessous |
| arret / `Ready` | 1 | `assdesasm` : Oricutron plante aussi sur Atmos |
| `ERROR` / OK | 10 | programmes ORIC-1, et 3 faux positifs (le mot fait partie du texte) |
| `ERROR` / `ERROR` | 2 | dont `ogp-nim.tap`, fichier abime (chaine de lignes BASIC cassee apres la ligne 710) |

**Contre-verification avec Oricutron 1.2.0** (lance sous Xvfb, memes cassettes,
ROM Phosphoric) :
- `osarkpan`, `oscomman`, `osqix`, `vache` : Oricutron s'arrete dans son
  debogueur **a la meme adresse** que Phosphoric (`$B5AC`, `$B5AC`, `$0808`,
  `$5057`) : defaut des fichiers, pas de l'emulateur. Les 10 cassettes `os*`
  arretees en `$B5AD` suivent le meme chemin (`JSR $023C` vers la ROM, puis
  retour sur un octet `$02` dans la zone du jeu de caracteres).
- Ecrans HIRES : les 282 captures sans texte lisible ont ete examinees a
  l'oeil sur planches contact (`tools/contact_sheet.py`). 23 etaient vides ou
  bloquees sur « patientez » ; 13 affichent leur titre avec un chargement plus
  long (80 M cycles). Sur les 10 restantes, 7 donnent le meme ecran sous
  Oricutron (dont 2 programmes ORIC-1 et 1 cassette qui attend un fichier
  suivant).

**Divergences non resolues** (pas de vrai ORIC pour trancher) :
- `LOADER2.TAP` sur ROM ORIC-1 : Phosphoric arrete le 6502 en `$B585` (aussi
  avec `--tape-signal`, lecture par la vraie ROM) ; Oricutron revient a `Ready`.
  Aucun des deux n'affiche le jeu.
- `ALCHIMIE.tap` sur ROM Atmos : Phosphoric execute le programme ; Oricutron
  plante dans la ROM (`$E786`, routines cassette, possible effet de son
  chargement rapide).

Limite : le critere est l'etat du CPU, l'ecran texte et, pour le HIRES, un
examen visuel sans reference ; un son faux ou un blocage plus tardif ne sont
pas detectes.

`make test-game-compat` lit aussi cette archive (`ORICLIB_TAP`, defaut
`~/oriclib/games/tap`) : 10 titres a ecran d'accueil connu, ignores si le
dossier est absent.

---

## Balayage de 484 disquettes (2.31.12)

`tools/dsk_sweep.sh DOSSIER [TACHES]` demarre chaque `.dsk` sur ROM Atmos + ROM
Microdisc pendant 30 M cycles, deux fois : timing reel du WD1793 (defaut, octets
au rythme du plateau depuis 2.31.9) et `--fdc-timing fast`. Lance sur
`~/oriclib/games/dsk` (archive locale, non versionnee) le 2026-11-01 :

| Reel / fast | Nombre | Lecture |
|-------------|--------|---------|
| OK / OK | 424 | |
| OK / octets perdus | 59 | le modele reel ne perd rien ; en `fast`, le chien de garde leve S2 sans perte reelle (octets servis a la demande du CPU) |
| arret / arret | 1 | `MazeEaterUTS.dsk` : programme ORIC-1 (meme arret en `$1108` que `01.tap` sur ROM Atmos, qui marche sur ROM 1.0) |

**Aucun octet perdu sur les 484 disquettes avec le modele lie au plateau.** Ecrans
(passage reel) : 398 avec du texte, 51 sur `Ready` (pas de demarrage automatique),
33 qui demandent une disquette systeme (disquettes de donnees), 1 vide. Limite :
« OK » veut dire « ni arret, ni perte, ni delai depasse », pas « programme juste ».

---

## Resultats des tests unitaires

`make tests` : **908 tests, 100% pass** (35 suites), verifie le 2026-08-30.

Le detail par suite evolue a chaque version ; la source faisant autorite
(compteur total + statut par composant) est maintenue dans **VERSION_TRACKING**
et **CIRRUS_OS** a la racine du depot. Voir aussi `make test-<suite>` (cpu,
memory, io, storage, system, video, audio, debugger, savestate, atmos,
joystick, printer, mcp40, renderer, trace, profiler, rominfo, serial, loci…).

---

## Compatibilite ROM

| ROM | Taille | Vecteur RESET | Modele | Statut |
|-----|--------|--------------|--------|--------|
| basic10.rom (BASIC 1.0) | 16384 octets | $EA59 | ORIC-1 | Valide |
| basic11b.rom (BASIC 1.1) | 16384 octets | $ECCC | Atmos | Valide |
| microdis.rom (Microdisc) | 8192 octets | N/A | Overlay $E000 | Fonctionnel |

---

## Precision de l'emulation

| Composant | Precision | Notes |
|-----------|----------|-------|
| 6502 CPU | Précis au cycle bus | 151 opcodes officiels, BCD, bug JMP indirect |
| VIA 6522 | Fonctionnel | Timers, interruptions, callbacks ports, CB1 edge |
| AY-3-8910 PSG | Precis | Courbe DAC Oricutron, diviseurs d'horloge |
| ULA Video | Fonctionnel | Texte + HIRES, attributs serie, timing PAL |
| WD1793 FDC | Fonctionnel | Commandes Type I/II, lecture/ecriture secteur ; saves in-game persistantes (--disk-writeback / savestate) |
| Clavier | Precis | Matrice 8x8 via VIA + PSG Port A |
| Joystick IJK | Fonctionnel | Port A PSG actif bas, clavier + gamepad |
| Imprimante | Fonctionnel | Centronics : donnée sur ORA, STROBE sur PB4, ACK sur CA1 (2.31.4 ; avant : CA2, faux) |
| MCP-40 | Fonctionnel | 8 commandes, 4 couleurs, export BMP |
| ACIA 6551 | Fonctionnel | Serie $031C-$031F, backends loopback/tcp/pty/com/modem |
| Digitelec DTL 2000 | Fidele | PIA 6821 + ACIA 6850 a $03F8-$03FD, registres OCR |
| MIDI Mageco | Fonctionnel | MC6850 a $03FE-$03FF, 31250 baud, `--mageco` (carte d'origine, forum t=2525 p.1) |
| MIDI ORICON | Fonctionnel | MC6850 a $031C-$031D + generateur d'horloge $031E-$031F, `--oricon` (reboot moderne, p.3, compatible LOCI) |
| MIDI temps reel | Fonctionnel | Port MIDI hote `--mageco midi[:TARGET]` (build `MIDI=1`) : ALSA (Linux, verifie), CoreMIDI/WinMM (ecrits, non verifies) |
| Lecteur .mid (SMF) | Fonctionnel | `--mageco smf:FILE[:loop]` rejoue un fichier .mid dans l'Oric en MIDI IN cadence (format 0/1, carte de tempo) |

---

## Limitations connues

- Le support Telestrat n'est pas encore implemente
- Certains programmes avec protection anti-copie peuvent ne pas se charger
- La couverture de code n'a pas ete mesuree formellement (estimation > 80%)
- Les tests visuels (rendu HIRES, couleurs) sont valides par screenshot headless
- Les ecritures disque du jeu (saves in-game) ne sont ecrites dans le fichier .dsk
  qu'avec --disk-writeback (opt-in, ecrase en place) ; sinon elles vivent dans la
  session ou dans un savestate (.ost capture l'image disque)
- La carte MIDI Mageko occupe $03FE/$03FF : sur vrai materiel, le forum t=2525
  signale un risque de conflit d'adresses avec d'autres extensions (l'emulateur
  avertit si le Microdisc est present). Le backend MIDI temps reel `--mageco midi`
  necessite un build `MIDI=1` ; la branche ALSA (Linux) est verifiee, les portages
  CoreMIDI (macOS) / WinMM (Windows) sont ecrits mais non verifies sur materiel reel

---

## Signaler la compatibilite

Si vous testez un programme non liste ici, merci de signaler :
- Nom du programme et format (.TAP/.DSK)
- S'il se charge et s'execute correctement
- Tout glitch visuel ou audio observe
- Etapes pour reproduire tout probleme
