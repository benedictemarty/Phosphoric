# Piloter l'émulateur Phosphoric depuis Claude Code

Guide destiné à **Claude Code** (ou tout agent) pour développer et **tester automatiquement**
un programme ORIC-1 / Atmos à l'aide de l'émulateur **Phosphoric** (`oric1-emu`).

> Copiez ce fichier à la racine (ou dans `docs/`) d'un nouveau projet Oric.
> Toutes les options citées proviennent de `./oric1-emu --help` — vérifiez-les avec
> `--help` si une version diffère. **Ne jamais inventer une option : la vérifier d'abord.**

---

## 1. Principe : tout se teste en *headless*

Phosphoric tourne **sans écran** (`-n / --headless`) et sait produire des artefacts
inspectables par un agent (texte, image, RAM, WAV, trace CPU). C'est ce qui rend un
programme Oric **auto-testable** en CI, sans intervention humaine.

Boucle type d'un test automatisé :

1. On boote une ROM + on charge le programme (tape `.tap`, disque `.dsk`, ou binaire).
2. On laisse tourner N cycles (`-c`) — ou on déclenche sur un état mémoire.
3. On **capture** un artefact (screenshot texte/PNG, dump RAM, WAV…).
4. On **vérifie** l'artefact (comparaison, motif, valeur mémoire) → exit code.

Le CPU tourne à 1 MHz, **19968 cycles par frame**, 50 frames/s (PAL).
Donc : `1 seconde émulée ≈ 1 000 000 cycles ≈ 50 frames`.

---

## 2. Commandes de base

```bash
# Boot BASIC 1.0 (ORIC-1) / BASIC 1.1 (Atmos)
./oric1-emu -r roms/basic10.rom
./oric1-emu -r roms/basic11b.rom

# Headless, tourne 5 s émulées puis quitte
./oric1-emu -r roms/basic10.rom -n -c 5000000

# Charger et lancer une cassette instantanément (pas de CLOAD manuel)
./oric1-emu -r roms/basic10.rom -t prog.tap -f

# Charger un disque Sedoric
./oric1-emu -r roms/basic10.rom --disk-rom roms/microdis.rom -d disque.dsk

# Choisir le modèle explicitement (sinon auto-détection depuis la ROM)
./oric1-emu -r roms/basic11b.rom -m atmos
```

Options de chargement utiles :

| Option | Rôle |
|---|---|
| `-r, --rom FILE` | ROM système (obligatoire en pratique) |
| `-t, --tape FILE` | cassette `.tap` |
| `-f, --fast-load` | injection directe de la tape (pas de `CLOAD`) |
| `-d / --disk1/2/3` | disque `.dsk` en lecteur A/B/C/D |
| `--disk-rom FILE` | ROM Microdisc (`microdis.rom`) |
| `-h, --hostfs PATH` | monte un répertoire hôte |
| `-m, --model` | `oric1` ou `atmos` |
| `-k, --keyboard` | `qwerty` (défaut) ou `azerty` |

---

## 3. Injecter des entrées clavier (`--type-keys`)

Pour piloter un programme sans humain :

```bash
# Taper une ligne BASIC puis Return, après ~2,7 M cycles (fin du boot)
./oric1-emu -r roms/basic10.rom -n -c 5000000 \
  --type-keys 2700000:'PRINT "HELLO"\n'
```

Échappements : `\n`=Return, `\e`=Esc, `\b`=Del, `\u \d \l \r`=flèches,
`\Cx`=Ctrl+x, `\Fx`=Funct+x, `\Lx`/`\Rx`=Shift+x, `\pN`=pause N secondes émulées.

- **`--type-keys` est répétable** : plusieurs occurrences sont séquencées par cycle
  d'armement croissant.
- Le rythme est **synchronisé sur le scan clavier réel** → aucune touche perdue, même si
  le programme scrute lentement.
- **Mieux que deviner le cycle** : armer sur un état mémoire avec
  `--type-keys-when A:V:TEXT` (déclenche quand `RAM[A]==V`, A et V en hexa).

```bash
# Attendre que le jeu soit prêt (RAM[$BC9A]==$52) avant de taper
./oric1-emu -r roms/basic10.rom -n -c 20000000 \
  --type-keys-when BC9A:52:'\n'
```

> Pour un timing déterministe des injections **et** pour la série réseau (modem/XMODEM),
> ajouter `--realtime` (cadence le headless à 50 Hz).

> **Déboguer un scanner clavier maison** (jeu ASM natif qui balaie la matrice VIA+PSG
> lui-même, sans la ROM) : `--kbd-scan-trace FILE` écrit une ligne par lecture VIA
> Port B (`$0300`) — `<cycle> col reg7 reg14 matrix PB3`. Deux causes classiques de
> « scan muet » deviennent visibles : `reg7` bit6=0 (Port A en sortie → PB3 forcé à 0)
> et `matrix`≠`FF` (touches pressées/maintenues). `--psg-trace` **exclut** reg 14/15
> (matrice), d'où cet outil dédié.

---

## 4. Capturer un résultat (le cœur du test auto)

### 4.1 Écran en texte (le plus simple à vérifier)

```bash
# Vide l'écran texte 40x28 ($BB80) en ASCII à la sortie
./oric1-emu -r roms/basic10.rom -n -c 5000000 --screenshot-text out.txt

# À un instant précis (répétable pour plusieurs instants)
--screenshot-text-at 3000000:step1.txt --screenshot-text-at 6000000:step2.txt

# Déclenché sur un état mémoire (exit 2 si jamais atteint)
--screenshot-text-when BC9A:52:ready.txt
```

### 4.2 Image (PNG/BMP/PPM) et ANSI couleur

```bash
--screenshot out.bmp                 # ou .ppm, à la sortie
--screenshot-at 6000000:frame.ppm    # à un cycle donné (répétable)
--screenshot-when A:V:FILE           # sur RAM[A]==V
--screenshot-ansi out.ans            # framebuffer en texte ANSI true-color (lisible en terminal)
```

### 4.3 Mémoire RAM (vérification déterministe)

```bash
--dump-ram-at 5000000:dump.bin       # 64 Ko à cycle >= C
--dump-ram-when A:V:dump.bin         # sur RAM[A]==V
```

### 4.4 Son PSG (AY-3-8910)

```bash
--audio-wav son.wav      # WAV 16-bit stéréo 44.1 kHz (headless)
--psg-trace psg.log      # log des écritures registres AY (reg 0-13) + cycle CPU
```

> Vérifier le son sans écoute humaine : `--audio-wav` puis analyse (ex.
> `ffmpeg -i son.wav -af volumedetect -f null -` pour prouver un signal non muet).

### 4.5 Vidéo / frames

```bash
--video demo.avi --video-fps 50      # AVI Motion-JPEG
--frame-dump DIR --frame-dump-interval 25
```

---

## 5. Provoquer un état sans jouer (pokes)

Utile pour amener un jeu à une scène précise de façon déterministe :

```bash
--poke-at 4000000:9611=01      # écrit RAM[$9611]=$01 après 4 M cycles (répétable)
--poke-when A:V:ADDR=VAL       # écrit RAM[ADDR]=VAL quand RAM[A]==V (tout en hexa)
```

---

## 6. Débogage

```bash
--debug / -D               # démarre dans le débogueur REPL (break 1re instruction)
-b, --breakpoint ADDR      # break quand PC atteint ADDR (hexa)
--trace trace.log          # trace complète des instructions CPU
--trace-ring N             # ne garde que les N dernières instr. (idéal pour un hang)
--trace-irq irq.log        # log des entrées IRQ + RTI
--symbols prog.sym         # table de symboles (.sym / .lab / .sym65)
--rom-info                 # analyse de la ROM
--gdb[=PORT]               # stub GDB distant (target remote :PORT)
--profile prof.txt         # profil de performance CPU
```

---

## 7. Enregistrement / rejeu déterministe

```bash
--record session.movie     # enregistre les entrées clavier
--replay session.movie     # rejeu déterministe (ignore le clavier live)
--save-state s.ost / --load-state s.ost
```

Le rejeu d'une `.movie` (couplé à `-c` fixe) donne des runs **reproductibles** — idéal en CI.

---

## 8. Squelette de test automatisé (à réutiliser)

```bash
#!/usr/bin/env bash
# tests/test_smoke.sh — exemple de test e2e headless
set -euo pipefail
EMU=./oric1-emu
ROM=roms/basic10.rom
OUT=$(mktemp -d)

# 1. Boot + tape le programme, capture l'écran quand il est prêt
$EMU -r "$ROM" -n -c 8000000 \
     -t build/prog.tap -f \
     --screenshot-text-when BC9A:52:"$OUT/screen.txt"

# 2. Vérifie le résultat attendu
if grep -q "READY" "$OUT/screen.txt"; then
    echo "PASS"; exit 0
else
    echo "FAIL — écran obtenu :"; cat "$OUT/screen.txt"; exit 1
fi
```

Codes de sortie utiles : les options `*-when` renvoient **exit 2** si l'état n'est jamais
atteint dans le budget de cycles → un `--screenshot-*-when` qui échoue fait échouer le test.

---

## 9. Pièges connus (retours d'expérience)

- **Fenêtre SDL noire** sur certaines machines/GPU → lancer avec `--render-software`.
- **Headless trop rapide** (~25-45× temps réel) casse la série réseau et le timing
  des injections → ajouter `--realtime`.
- **`--screenshot-at` avec une seule variable** : le *dernier* déclencheur gagne. Pour
  plusieurs instants, utiliser plusieurs `--screenshot-*-at`/`-when` (ils sont répétables).
- **Objets stale après changement de mode de build** (`make SDL2=0` puis `make tests`) :
  toujours revalider via `make clean && make tests`.
- **Fenêtre de cycles** : trop court = programme pas encore prêt ; préférer `*-when` sur un
  état mémoire plutôt que deviner un cycle absolu.

---

## 10. Aide-mémoire touches (mode GUI)

`F1` aide · `F2` save-state · `F3` scale · `F4` load-state · `F5` reset · `F6` OSD
média à chaud · `F9` débogueur · `F10` quitter · `F11` plein écran · `F12` screenshot.

---

## Références projet

- `README.md` — référence CLI complète.
- `docs/user-guide/README.md` — guide utilisateur.
- `docs/control_protocol.md` / `docs/http-api.md` — pilotage IPC / REST.
- `docs/loci.md`, `docs/SEDORIC.md` — LOCI et filesystem Sedoric.
- `CLAUDE.md` — conventions du dépôt Phosphoric.
