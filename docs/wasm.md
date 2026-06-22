# Build WebAssembly (navigateur)

Phosphoric compile en **WebAssembly** via Emscripten : l'émulateur complet
(CPU 6502, mémoire, VIA, PSG, ULA, clavier, cassette, disque) tourne dans un
onglet, rendu sur un `<canvas>`, audio via Web Audio, clavier via le DOM — en
réutilisant le chemin SDL2 existant (port SDL d'Emscripten).

## Prérequis

[Emscripten SDK](https://emscripten.org) actif (`emcc` dans le `PATH`) :

```bash
git clone https://github.com/emscripten-core/emsdk
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
```

## Construire et lancer

```bash
make wasm
(cd web && python3 -m http.server 8000)
# ouvrir http://localhost:8000/phosphoric.html
```

`make wasm` produit dans `web/` : `phosphoric.html` (page + canvas),
`phosphoric.js`, `phosphoric.wasm`, et `phosphoric.data` (les ROMs de `roms/`
préchargées dans le système de fichiers virtuel). La page démarre l'Atmos
(`-r /roms/basic11b.rom`) ; cliquez l'écran pour le focus clavier et l'audio.

## Interface (page web)

La page (`web/shell.html`) présente un **rail d'icônes vertical à gauche**
(façon JOric) et le clavier ORIC en overlay :

- **MODEL** — bascule machine ORIC-1 / Atmos (badge `1`/`A`, relance à froid avec
  la ROM choisie).
- **LOAD** + **glisser-déposer** d'un `.tap`/`.dsk` sur l'écran : le fichier est
  inséré et la machine redémarre dessus (cassette `-t …-f`, ou disquette
  `--disk-rom microdis.rom -d …`). Bouton **EJECT** pour le retirer.
- **RESET** — reboot à froid en conservant ROM et média.
- **KEYS** — affiche/masque le clavier virtuel.
- **FULL** — plein écran (le canvas est centré et mis à la hauteur de l'écran,
  ratio 240/224 conservé).
- **Clavier virtuel ORIC fidèle**, en **overlay semi-transparent par-dessus
  l'écran** : disposition réelle (ESC, CTRL, FUNCT, 2× SHIFT, RETURN, DEL, SPACE,
  flèches ↑←↓→) avec modificateurs **collants CTRL / FUNCT / SHIFT**. Les
  symboles shiftés affichés en exposant sont **dérivés de la matrice réelle**
  (`char_map`) : `2`→`@`, `6`→`^`, `-`→`_`, `;`→`:`, `[`→`{`, `\`→`|`, etc.
  En **ORIC-1**, la touche **FUNCT (Atmos-only) est absente**.

> **CTRL+T et autres chords :** le navigateur réserve certains raccourcis
> (CTRL+T = nouvel onglet) au niveau de l'OS — ils n'atteignent jamais le
> canvas. Utilisez la **touche CTRL du clavier virtuel** : elle écrit la
> matrice ORIC via un appel C direct (`web_key`), donc le navigateur ne
> l'intercepte pas. Idem pour FUNCT.

> Servez les fichiers par **HTTP** (pas `file://`) : le navigateur refuse de
> charger un `.wasm` depuis le système de fichiers local.

## Détails techniques

- **Boucle principale** : sous le navigateur, la boucle `while` C doit rendre
  la main à la boucle d'événements à chaque frame. C'est fait via
  **Asyncify** (`-sASYNCIFY`) + `emscripten_sleep()` dans le limiteur de frame
  (`src/main.c`, gardé par `__EMSCRIPTEN__`) — qui cadence aussi à ~50 Hz.
- **Pile** : `emulator_t` est volumineux (framebuffer + mémoire) et vit sur la
  pile de `main()` ; le build force `-sSTACK_SIZE=8MB` (le défaut 64 Ko
  déborderait).
- **Réseau** : les fonctionnalités qui exigent des sockets/threads natifs
  (backends série TCP/PTY/COM, stub GDB, serveur Cast, terminaison TLS) se
  lient en no-op dans le navigateur — le cœur machine, la vidéo, l'audio, le
  clavier, la cassette et le disque fonctionnent.

## Fidélité — vérifié

La sortie WASM est **byte-identique au build natif** pour des entrées
identiques : un boot Atmos headless (`-n -c N --screenshot`) compilé en WASM et
exécuté sous Node.js produit la **même capture PPM exacte** que le binaire natif
(testé à 2M et 5M cycles). Le déterminisme cycle-exact du cœur est préservé à
travers la compilation WebAssembly.

Le rendu **navigateur** a aussi été validé : la page chargée dans Chromium
headless (`--virtual-time-budget`) affiche le canvas avec l'écran de boot Atmos
correct (« ORIC EXTENDED BASIC V1.1 / © 1983 TANGERINE / 37631 BYTES FREE /
Ready » + indicateur CAPS) — le chemin SDL2 → WebGL/canvas fonctionne de bout
en bout. La **saisie clavier** a aussi été validée : taper `PRINT 6*7` + RETURN
via le pont `web_key` (clavier virtuel) affiche `42` — boot ROM → injection
clavier → exécution BASIC → rendu, entièrement dans le navigateur.
