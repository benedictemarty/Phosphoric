# Liste de Compatibilite — Phosphoric v1.13.0-alpha

Derniere mise a jour : 2026-03-02

---

## Systemes d'exploitation

| Programme | Format | Modele | Statut | Notes |
|-----------|--------|--------|--------|-------|
| BASIC 1.0 ROM | .ROM | ORIC-1 | Fonctionnel | Interpreteur BASIC complet |
| BASIC 1.1 ROM | .ROM | Atmos | Fonctionnel | Auto-detection via JMP $ECCC |
| Sedoric V4.0 | .DSK | ORIC-1/Atmos | Fonctionnel | Boot, clavier, commandes DOS |
| Sedoric V3.0 | .DSK | ORIC-1/Atmos | Fonctionnel | Boot et commandes DOS |

---

## Jeux cassette (.TAP) — 25/25 fonctionnels

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

## Resultats des tests unitaires

| Sous-systeme | Tests | Statut |
|-------------|-------|--------|
| CPU 6502 | 74/74 | Tous passent |
| Memoire | 19/19 | Tous passent |
| VIA 6522 I/O | 29/29 | Tous passent |
| Stockage (Sedoric/FDC) | 12/12 | Tous passent |
| Integration systeme | 7/7 | Tous passent |
| Export video | 11/11 | Tous passent |
| Audio PSG | 8/8 | Tous passent |
| Debogueur | 8/8 | Tous passent |
| Sauvegarde d'etat | 8/8 | Tous passent |
| Atmos | 10/10 | Tous passent |
| Joystick | 10/10 | Tous passent |
| Imprimante | 10/10 | Tous passent |
| Traceur MCP-40 | 10/10 | Tous passent |
| Rendu / Scaling | 10/10 | Tous passent |
| Trace CPU | 10/10 | Tous passent |
| Profileur CPU | 10/10 | Tous passent |
| Analyse ROM | 10/10 | Tous passent |
| **Total** | **256/256** | **100%** |

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
| 6502 CPU | Cycle-accurate | 151 opcodes officiels, BCD, bug JMP indirect |
| VIA 6522 | Fonctionnel | Timers, interruptions, callbacks ports, CB1 edge |
| AY-3-8910 PSG | Precis | Courbe DAC Oricutron, diviseurs d'horloge |
| ULA Video | Fonctionnel | Texte + HIRES, attributs serie, timing PAL |
| WD1793 FDC | Fonctionnel | Commandes Type I/II, lecture/ecriture secteur ; saves in-game persistantes (--disk-writeback / savestate) |
| Clavier | Precis | Matrice 8x8 via VIA + PSG Port A |
| Joystick IJK | Fonctionnel | Port A PSG actif bas, clavier + gamepad |
| Imprimante | Fonctionnel | Centronics, STROBE via CA2 |
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
