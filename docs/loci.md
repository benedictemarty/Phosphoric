# LOCI — Lovely Oric Computer Interface

Émulation du périphérique LOCI de **Sodiumlightbaby** (sodiumlb, 2024) : une
cartouche RP2040 qui se branche sur le bus de l'Oric et fournit stockage de
masse (USB / SD / flash interne), clavier-souris-manettes USB HID, modem WiFi
(PicoWiFiModemUSB), swap de ROM à chaud et menu intégré.

Références : [loci-hardware](https://github.com/sodiumlb/loci-hardware) ·
[loci-firmware](https://github.com/sodiumlb/loci-firmware) ·
[loci-rom](https://github.com/sodiumlb/loci-rom) (menu). L'émulation est
alignée sur la source du firmware (release de référence : **v0.3.1**) et
vérifiée sur pièces ; les écarts connus sont listés en fin de document.

## Démarrage rapide

```bash
# LOCI + modem WiFi picowifi (ACIA 6551 à $0380, adressez $0380 PAS $03A0)
./oric1-emu -r roms/basic11b.rom --loci --serial picowifi

# Menu LOCI directement au boot
./oric1-emu -r roms/loci/locirom --loci

# Image SD FAT16/32 brute comme stockage
./oric1-emu -r roms/basic11b.rom --loci --loci-sdimg carte.img
```

### Une carte, trois modes (menu F1)

Dans le menu F1, la carte **LOCI** a un réglage **Mode** ; seuls les réglages du mode
choisi sont affichés :

| Mode | Équivaut à | Réglages |
|------|-----------|----------|
| `intégré` | `--loci` | menu au démarrage, image SD, dossier flash, modem picowifi |
| `firmware` | `--loci-emu ELF` | firmware ELF (obligatoire), image flash, image de clé USB |
| `usb` | `--loci-hw PORT` | port de la LOCI-USB (Feather) |

Un même binaire contient les trois modes : `firmware` existe quand `~/loci/emul`
(libemul) était là à la compilation, `usb` quand `~/loci/loci-usb` y était (pas
sous Windows). Le menu ne propose que les modes présents. Les backends sont choisis
au lancement (`src/io/loci_backend.c`). `make LOCI_EMU=0` ou `LOCI_HW=0` en retire
un.

### LOCI-USB : la Feather (`--loci-hw`, mode `usb`)

Une **LOCI-USB** est une LOCI sans interface Oric (ni CN1 ni translateurs), avec un
second USB : aujourd'hui une Adafruit Feather RP2040 USB Host (5723), demain la carte
LOCI-USB (`~/loci/loci-usb`). Elle fait tourner le firmware LOCI lui-même (variante
`LOCI_USB`, ou loci-fw-usb) ; ce n'est **pas** un pont vers une cartouche LOCI 1.3.
Phosphoric joue l'Oric : son 6502 émulé envoie ses accès `$03xx` et ROM par l'USB-C,
et le firmware les rejoue dans son chemin de bus habituel ; le port USB-A sert de
ports USB de la LOCI (clé, clavier, modem). Elle ne s'enfiche pas sur un vrai Oric :
pour un Oric physique, il faut la LOCI 1.3. Recette :
`~/loci/loci-usb/docs/RECETTE-FEATHER.md`.

```bash
./oric1-emu --loci-hw /dev/ttyACM0
```

Laissé vide, le réglage **Port de la LOCI-USB** est détecté sous Linux par le
nom de produit USB, qui commence par « LOCI-USB » (firmware `feature/loci-usb` ou
loci-fw-usb). La détection lit seulement `/sys`, sans ouvrir le port. Si aucune LOCI-USB
n'est détectée et qu'aucun port n'est indiqué, le menu le signale et refuse
d'appliquer. Dans `phosphoric.cfg` : `carte.loci=oui`, `loci.mode=usb`,
`loci.port_usb=/dev/ttyACM0`. Les anciennes clés `carte.loci_emu`, `loci_emu.*`,
`carte.loci_hw`, `loci_hw.port`, `loci.pont` et la valeur `loci.mode=réelle` sont
encore lues.

### Dans le navigateur (build WebAssembly)

`phosphoric.html?loci=1` (ou le bouton **LOCI** du rail) démarre sur le menu
LOCI, avec un flash interne persistant dans IndexedDB ; les fichiers chargés y
sont copiés et se montent depuis le menu. Détails et limites (pas de picowifi,
de clés USB ni d'image SD en web) : [wasm.md](wasm.md).

## Cartographie mémoire

| Fenêtre | Contenu |
|---------|---------|
| `$0310-$031F` | WD1793 + contrôle DSK (mode Microdisc du LOCI ; `$0319` = 'L') |
| `$0315-$0317` | Protocole TAP bas niveau (PLAY/REC/READ_BIT, trame 14 bits) |
| `$0380-$0383` | ACIA 6551 (modem picowifi) — défaut sous `--loci` |
| `$03A0-$03BF` | MIA : console UART, registres API (xstack `$03AC`, errno `$03AD/E`, op `$03AF`), stub `$03B0` (spin/BLOCKED), BUSY `$03B2` bit 7, trap bouton `$03BA-$03BF` |

## API (op `$03AF`) — 36/36 ops implémentées

Système (`PIX_XREG`, `CPU_PHI2`→1000 kHz, `OEM_CODEPAGE`, `RNG_LRAND`,
`STDIN_OPT`), horloge (`CLOCK`, `CLK_GET/SETTIME`, `GETRES`), fichiers
(`OPEN/CLOSE/READ/WRITE_XSTACK/XRAM/LSEEK/UNLINK/RENAME`), répertoires
(`OPENDIR/CLOSEDIR/READDIR/MKDIR/GETCWD`), montage (`MOUNT/UMOUNT`, TAP
`SEEK/TELL/READ_HEADER`, `UNAME`), boot/tuning (`MIA_BOOT`, `MAP_TUNE_*`,
`ADJ_SCAN`), sentinelle `$FF` (exit → spin).

### ABI conforme au firmware (vérifiée sur source)

- **errno** : erreurs filesystem = `32 + FRESULT` FatFS (fichier manquant → 36,
  répertoire manquant → 37, refusé/non-vide/plein → 39, existe déjà → 40…) ;
  les codes 1-18 sont réservés aux erreurs API (`EBADF`, `EMFILE`, `ENODEV`,
  `ENOSYS`, garde anti-échappement) — exactement comme `api.h` du firmware.
- **Descripteurs** : fichiers 3-18 (FAT, `STD_FIL_OFFS=3`), répertoires 64+
  (`FD_OFFS_FAT`) ; l'itérateur de périphériques est le fd 0 (`FD_OFFS_DEV`).
- **xstack** : 512 octets, push/pop et chaînes sans NUL conformes.
- **`MAP_TUNE_*`** : valeur dans le registre A ; A ≤ 31 règle le délai, toute
  autre valeur est une *requête* ; l'op renvoie toujours la valeur courante
  dans AX. `ADJ_SCAN` balaye tior 0-31 (~100 ms + 5 ms/pas) avec progression
  visible dans l'octet ROM `$FFF0` (`0x80|tior` puis tior configuré).

## Stockage

Le vrai LOCI a trois étages ; leurs équivalents dans Phosphoric :

| Matériel réel | Émulation |
|---------------|-----------|
| **Flash interne** (LittleFS du RP2040, pré-semée `basic11b.rom`, `basic10.rom`, `microdis.rom`, `locirom`) | **flash root** : `--loci-flash DIR` (défaut : répertoire courant). Chemins `0:` et nus. Les ROM système absentes du flash root sont résolues en repli dans le dossier de la ROM `-r` (donc `roms/`) |
| **Clé USB** (FAT, port USB host) | `--loci-usb DIR` (répétable, 4 max) **ou auto-détection** des médias montés dans `/media/$USER` et `/run/media/$USER` au lancement. Chemins volume `1:`-`4:`. Label + taille affichés dans le menu (`N: MSC x.x GB <label>`). Pas de hot-plug : brancher avant de lancer |
| **Carte SD** (image brute) | `--loci-sdimg PATH` (FAT16/32). NOTE : quand actif, ce backend possède toutes les ops fichiers (les clés USB restent listées mais non navigables) |

La **liste des périphériques** (sélecteur du menu) est servie par
`opendir("")` : « 0: Internal storage [15MB] », puis une ligne par
périphérique USB (`usb_set_status` du firmware — la clé MSC, le picowifi
« CDC modem mounted »), puis un nom vide.

## Bouton Action (F8)

Comportement du firmware (`ext.c`) reproduit :

- **Appui court** : snapshot de la session (→ `<flash root>/loci_resume.ost`),
  trap IRQ `$03BA` (CLV; BVC -2; JMP ($FFFA)), puis **boot du menu LOCI**
  (`LOCIROM`/`locirom` du flash root, repli `roms/loci/locirom`). Comme le
  vrai firmware, la version FW (0.3.1) et les timings (tmap/tior/tiow/tiod/
  tadr) sont **patchés dans la ROM** aux placeholders `$FFF7-9` / `$FFEF-F3`.
  L'entrée *resume* du menu (`MIA_BOOT` + `LOCI_BOOT_RESUME`) re-swape la ROM
  d'avant et restaure le snapshot. Appuyer sur F8 dans le menu est ignoré
  (le snapshot de session est préservé).
- **Appui long (≥ 2 s)** : boot de la **diag ROM de Mike Brown**
  (`roms/loci/test108k.rom`, v1.08k, incluse dans les builds firmware réels
  avec sa permission — variante 60 Hz fournie). Test pas-à-pas
  CPU/ULA/DRAM/VIA/PSG.
- En mode `--control` : commande `loci-button [long]`.
- F5 = reset MIA (registres/xstack/op) en conservant les montages, comme le
  bouton reset du Pico.

## Timing du bus MIA

La MIA échantillonne le bus 6502 par PIO à des décalages sous-cycle réglés
par `MAP_TUNE_*`. Un `tior` mal calé corrompt la fenêtre ACIA du picowifi —
symptôme matériel réel reproduit : `--loci-mia-window LO-HI` définit la
plage fiable (défaut 0-31 = toujours fiable) ; hors fenêtre, `$0380` lit
`$FF` et ignore les écritures.

`--loci-serve-timing SERVE[,TDSR]` remplace la fenêtre par la chronologie en ns de
`bus_timing.h` (la même que `--loci-hw`, ci-dessous) : SERVE en cycles du cœur 1 de
la LOCI (23 mesurés sur matériel), TDSR en ns (100). `tior` et `tiod` y entrent ;
`--loci-serve-jitter AMP[,SEED]` ajoute ± AMP cycles, seedé. Frontière par défaut :
81/82 cycles de serve. Détails : [`architecture/phi2-bus-timing.md`](architecture/phi2-bus-timing.md).

### Boîte aux lettres BAL de loci-fw (`--loci-hw`)

Avec le firmware **loci-fw** (variante LOCI_USB, caps `FIRMWARE`), le 6502 appelle
l'API en écrivant en page `$FF` (`$FF00-$FFCF`). `--loci-hw` transmet ces écritures
(`WR`) ; une écriture captée revient avec `LUP_F_SERVED`, l'octet est recopié dans
le cache ROM sans le recharger. Un octet non nul en `$FF00` lance une commande :
Phosphoric relit `$FF00` (`RD` non caché) jusqu'à 0, ce qui évite d'attendre le
prochain `LINES` (10 s au plus, `LOCI_HW_BAL_TIMEOUT_MS`). Exception : le groupe 2
(Console) n'est jamais traité par le firmware, c'est le kernel 6502 qui l'exécute
(ADR-004 de loci-fw) ; Phosphoric ne l'attend donc pas. Le dispatcher change `gen8` en rendant ses résultats, le cache
ROM est alors rechargé. L'ancien firmware ne capte jamais ces écritures : rien ne
change pour lui. Bilan en fin de session : écritures captées, commandes.

### Course Φ2 sur la LOCI-USB (`--loci-hw`)

Avec `--loci-hw`, chaque accès `$03xx` est un aller-retour USB pendant lequel le
6502 émulé est figé : la contrainte Φ2 d'une vraie LOCI (le port d'extension n'a
pas de RDY, la donnée doit être posée avant que le 6502 la capture) disparaît. Quand
le firmware annonce `caps & LUP_CAP_TIMING` (loci-usb, commandes TIMING/RDT/WRT),
Phosphoric la reconstitue à partir des cycles du cœur 1 mesurés au SysTick (`serve`,
`act`), sur une chronologie en ns dont l'origine est le front descendant de Φ2 :

| Étape | Instant | Source |
|---|---|---|
| mot d'action dans la FIFO | (22 + tior) ticks PIO + 2 cycles sys | comptes de `mia.pio` (estimés) |
| donnée prête (DMA déclenchée) | + `LOCI_HW_POLL_NS` (0, non mesuré) + `serve` | SysTick |
| donnée sur le bus | max(prête, montée de Φ2) + (3 + tiod) ticks | `mia_io_read` (estimé) |
| échéance du 6502 | période − `LOCI_HW_TDSR_NS` (100) | fiche 6502 à 1 MHz |

- **Tick PIO** = 1 / (Φ2cfg × 30), où Φ2cfg est le réglage du firmware (**4000 kHz
  par défaut**, `cpu.c`) : 8,33 ns. Ce n'est pas l'horloge de l'Oric.
- **Φ2 de l'Oric** : période 1 / `LOCI_HW_PHI2_KHZ` (1000), haut le dernier tiers
  (`LOCI_HW_PHI2_HIGH_NS`, période / 3 : l'ULA donne 2/3 bas, 1/3 haut).
- **Lecture en retard** : la donnée arrive après l'échéance. **Iopage périmé** : un
  accès `$03xx` arrive moins de `act` cycles 6502 après le précédent (granularité :
  l'instruction).

Détection seule par défaut : journal des 10 premiers cas + bilan en fin de session
(marge minimale en ns). `LOCI_HW_FAITHFUL=1` rend l'open-bus sur une lecture en
retard. Mesures sur la Feather 5723 : serve 23 cycles → donnée à ≈ 708 ns, juste
après la montée de Φ2, marge ≈ 190 ns ; act 61-118 cycles en lecture, 429 pour une
écriture RAMX (3,6 µs : un accès `$03xx` moins de 4 cycles 6502 après lit un iopage
périmé). Les comptes PIO et le temps d'établissement du 6502 à 2 MHz de l'Oric sont
des estimations, à confirmer par une mesure sur bus réel.

## Divergences connues (hors périmètre)

- Flash interne LittleFS non émulée en tant que telle (fichiers LFS 19-20,
  répertoires 32+, errno `128-lfs_err`, volume « 0: » réel) — le flash root
  joue ce rôle avec la sémantique FAT.
- Répertoire dev « 0 » du firmware, pattern matcher ULA réel, BUSY observable
  pendant une op longue.
- Détection USB au démarrage uniquement (pas de hot-plug).
- Le `dsk_fdc` du LOCI reste en timing FDC rapide — fidèle : sur le vrai
  matériel son « lecteur » est le RP2040 + SD, sans mécanique (le Microdisc,
  lui, est en timing mécanique réel par défaut).

## Pour aller plus loin

- **Article de vulgarisation** — [Cinq extensions expérimentales pour la carte
  LOCI](articles/loci-extensions-tachibana.md) (ABI *fastcall*, carte des registres
  MIA, schémas) : coprocesseur `$A9`, ACIA fiable `$AA`, banque `$A7`, streamer
  `$A8`, modèle de *tearing*. Ces extensions sont **expérimentales, opt-in et hors
  `main`** (branche `experiment/loci-coproc-acia-reliable`).
- **Architecture interne** — [`architecture/loci-glue.md`](architecture/loci-glue.md),
  [`architecture/phi2-bus-timing.md`](architecture/phi2-bus-timing.md).
