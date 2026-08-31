# Outils de Phosphoric

Utilitaires en ligne de commande livrés avec l'émulateur (dossier `tools/`).
Ils couvrent la conversion de programmes ORIC (BASIC / binaire → cassette),
la fabrication et l'inspection de disquettes Sedoric, et l'export vers des
formats « physiques » (audio cassette, flux magnétique disquette).

## Compilation

```bash
make tools          # construit tous les outils C ci-dessous
```

Chaque outil se compile aussi seul, par ex. `make tap2wav`. Les scripts Python
et shell ne nécessitent pas de compilation.

| Outil            | Entrée → sortie            | Rôle                                             |
|------------------|----------------------------|--------------------------------------------------|
| `bas2tap`        | `.bas` → `.tap`            | Programme BASIC texte → cassette                 |
| `bin2tap`        | `.bin` → `.tap`            | Binaire code machine → cassette (load/exec)      |
| `tap2sedoric`    | `.tap` (+ base `.dsk`) → `.dsk` | Injecte un `.tap` dans une disquette Sedoric |
| `sedoric-info`   | `.dsk` → texte             | Inspecte VTOC / catalogue d'une disquette Sedoric |
| `tap2wav`        | `.tap` → `.wav`           | **Cassette audio** (rejouable sur vraie machine) |
| `dsk2hfe`        | `.dsk` (MFM_DISK) → `.hfe` | **Image magnétique HFE** (HxC / Gotek / Greaseweazle) |

Scripts d'appoint : `dsk_raw2mfm.py`, `sedoric_inject.py`, `sedoric_mkbare.py`,
`make_bootable_sedoric.sh` (voir plus bas).

---

## bas2tap — BASIC texte → cassette

Convertit un listing BASIC ORIC (texte) en fichier `.tap` (jetonisé, en-tête
cassette 9 octets compatible ROM).

```
bas2tap <input.bas> -o <output.tap> [--auto-run]
```

- `--auto-run` : positionne le drapeau d'auto-exécution (RUN automatique après
  `CLOAD`).

```bash
bas2tap jeu.bas -o jeu.tap --auto-run
```

## bin2tap — binaire → cassette

Empaquette un binaire code machine dans un `.tap` avec adresses de chargement et
d'exécution.

```
bin2tap <input.bin> --start <addr> --exec <addr> -o <output.tap> [--name <name>] [--no-autorun]
```

- `--start` : adresse de chargement (hex, ex. `0x500`).
- `--exec`  : adresse d'exécution (auto-lancée après chargement).
- `--name`  : nom du fichier cassette (défaut `PROGRAM`).
- `--no-autorun` : force le drapeau d'auto-run à `$00`.

```bash
bin2tap demo.bin --start 0x9800 --exec 0x9800 -o demo.tap --name DEMO
```

## tap2sedoric — injecter un .tap dans une disquette Sedoric

Copie une image `.dsk` de base (format **MFM_DISK**) et y injecte le contenu
d'un `.tap` comme fichier Sedoric. Voir `docs/SEDORIC.md` pour le format des
descripteurs.

```
tap2sedoric <input.tap> -o <output.dsk> -b <base.dsk> [-n NAME.EXT] [-a] [-e EXEC_HEX] [-i "INIST"]
```

- `-b` : disquette de base (doit être un conteneur MFM_DISK).
- `-n` : nom du fichier sur la disquette (ex. `JEU.COM`).
- `-a` : marque le fichier AUTO (auto-lancé).
- `-e` : adresse d'exécution (hex) pour un `.COM`.
- `-i` : chaîne INIST (personnalise le boot).

```bash
tap2sedoric jeu.tap -o jeu.dsk -b master.dsk -n JEU.COM -a
```

## sedoric-info — inspecter une disquette Sedoric

Affiche les compteurs VTOC, le catalogue, et peut servir de garde-fou de
non-régression.

```
sedoric-info <disk.dsk> [--check FREE:FILES]
```

- `--check FREE:FILES` : sort en erreur si le nombre de secteurs libres ou de
  fichiers diffère des valeurs attendues (utile en CI).

```bash
sedoric-info jeu.dsk
sedoric-info jeu.dsk --check 640:3
```

---

## tap2wav — cassette audio (`.tap` → `.wav`)

Produit le **vrai signal cassette ORIC** en `.wav`, rejouable sur l'entrée K7
d'une machine réelle (via un câble jack/DIN) ou archivable en audio.

```
tap2wav IN.tap OUT.wav [--rate HZ] [--leader N] [--amp N] [--lead-silence MS] [--tail-silence MS]
```

| Option           | Défaut  | Effet                                              |
|------------------|---------|----------------------------------------------------|
| `--rate`         | 44100   | Fréquence d'échantillonnage du WAV (Hz)            |
| `--leader`       | 512     | Nombre de trames de synchro `0x16` en tête         |
| `--amp`          | 16000   | Amplitude crête (int16)                            |
| `--lead-silence` | 200     | Silence avant le signal (ms)                       |
| `--tail-silence` | 500     | Silence après le signal (ms)                       |

**Encodage** (repris à l'identique du générateur signal de l'émulateur,
`src/io/cassette.c`, calqué sur la ROM CSAVE `$E619`) : trame de 14 bits,
LSB d'abord — `start(0) · 8 data · parité impaire · 4 stop(1)`. Chaque bit est
un demi-pulse LOW de 208 cycles puis un demi-pulse HIGH de 208 cycles (bit `1`)
ou 416 cycles (bit `0`), à 1 MHz. Le décodeur ROM sépare `1`/`0` au seuil
~512 cycles.

```bash
make tap2wav
./tap2wav jeu.tap jeu.wav
# puis : jouer jeu.wav dans l'entrée cassette de l'ORIC, taper CLOAD"" côté machine
```

## dsk2hfe — image magnétique disquette (`.dsk` → `.hfe`)

Convertit une disquette ORIC au format **MFM_DISK** en image **HFE v1**
(`HXCPICFE`), le format flux magnétique lu/écrit par un **HxC Floppy Emulator**,
un **Gotek** sous FlashFloppy, ou un **Greaseweazle** — l'équivalent disque de
`tap2wav` pour écrire de vraies disquettes.

```
dsk2hfe IN.dsk OUT.hfe [--bitrate KBPS] [--rpm RPM]
```

| Option      | Défaut | Effet                          |
|-------------|--------|--------------------------------|
| `--bitrate` | 250    | Débit cellule / 2 (kbit/s)     |
| `--rpm`     | 300    | Vitesse de rotation (RPM)      |

**L'entrée doit être un conteneur MFM_DISK** (celui d'Oricutron/Phosphoric :
en-tête `MFM_DISK`, pistes brutes de 6400 octets). Pour une image plate/brute,
la convertir d'abord avec `dsk_raw2mfm.py`. L'outil MFM-encode chaque piste en
flux de cellules (16 cellules/octet, paires clock/data), avec les marques de
synchro standard A1=`0x4489` et C2=`0x5224` (clock manquant) devant les address
marks ; sortie conforme à la spec HxC (en-tête 512 o, table des pistes, faces
entrelacées par blocs de 256 o, bits **LSB d'abord**).

```bash
make dsk2hfe
./dsk2hfe jeu.dsk jeu.hfe
# copier jeu.hfe sur la carte SD du HxC/Gotek, ou l'écrire avec Greaseweazle
```

---

## Scripts d'appoint

| Script                     | Rôle                                                         |
|----------------------------|-------------------------------------------------------------|
| `dsk_raw2mfm.py`           | Image disque **brute** (secteurs concaténés, ex. `--disk-create`) → conteneur **MFM_DISK** |
| `sedoric_inject.py`        | Injecte un binaire comme fichier SEDORIC dans une image     |
| `sedoric_mkbare.py`        | Fabrique un master Sedoric « nu » (INIST neutralisé)        |
| `make_bootable_sedoric.sh` | Chaîne complète : disquette Sedoric **bootable** hands-free |

Voir `docs/SEDORIC.md` pour le format Sedoric et les recettes de chargement.

---

## Chaînes de conversion typiques

**Programme BASIC → disquette bootable**
```bash
bas2tap jeu.bas -o jeu.tap --auto-run
tap2sedoric jeu.tap -o jeu.dsk -b master.dsk -n JEU -a
```

**Vers du matériel réel**
```bash
tap2wav jeu.tap jeu.wav       # charger par l'entrée cassette
dsk2hfe jeu.dsk jeu.hfe       # écrire une vraie disquette (HxC/Gotek/Greaseweazle)
```

**Image brute → MFM → HFE**
```bash
python3 tools/dsk_raw2mfm.py brut.dsk mfm.dsk
dsk2hfe mfm.dsk sortie.hfe
```
