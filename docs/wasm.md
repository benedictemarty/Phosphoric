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
en bout.
