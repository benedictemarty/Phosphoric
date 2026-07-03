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
