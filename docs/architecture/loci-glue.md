# Architecture — Adaptateur LOCI (`src/io/loci_glue.c`)

Livré par l'**Epic 9** (v1.99.5 → v1.99.8). Documente l'adaptateur qui relie le
cœur LOCI à l'émulateur, sur le modèle de `io-bus.md`.

## 1. Le problème

Le LOCI (carte sodiumlb : WiFi modem + stockage SD + ROM-swap, exposée en MIA
`$03A0-$03BF`) a besoin d'atteindre **tout** l'émulateur : mémoire, CPU, vidéo,
OSD, clavier, disques, tape. Mais son cœur (`src/io/loci_*.c`) est délibérément
gardé **pur** — il n'inclut pas `emulator.h`. Il en résultait ~15 fonctions
`loci_*` de « glue » entassées dans `main.c` (~400 l), mêlées au reste de
l'assemblage.

## 2. La solution — un adaptateur dédié

`src/io/loci_glue.c` est le **seul** fichier qui connaît à la fois le LOCI et
`emulator_t` — exactement le rôle d'adaptateur de `io_bus.c`. Il concentre les
16 fonctions de glue ; `main.c` n'en garde que le **câblage**.

## 3. Le contrat (le seam)

Le cœur LOCI expose une API d'enregistrement de callbacks prenant un
`void* ctx` opaque :

```c
loci_set_rom_swap_callback(&emu.loci, loci_rom_swap_cb, &emu);
loci_set_dsk_bus_callbacks(&emu.loci, loci_dsk_cpu_irq_set, ..., &emu);
loci_set_action_callbacks(&emu.loci, loci_action_install_irq_trap, ...);
```

Chaque callback fait `emulator_t* emu = (emulator_t*)ctx;`. Ce seam rend
l'extraction **mécaniquement triviale** : déplacer le corps, laisser
l'enregistrement dans `main.c`. C'est pourquoi tout l'Epic est resté
byte-identique.

## 4. Répartition

| Dans `loci_glue.c` (l'adaptateur) | Reste dans `main.c` (le câblage) |
|-----------------------------------|----------------------------------|
| Les 16 fonctions `loci_*` (callbacks bus/IRQ, ROM/tape/resume, USB, IRQ-trap, sync clavier SDL) | `loci_init(&emu.loci)`, les `loci_set_*_callback(...)`, les appels de setup (`loci_attach_usb_dir`, `loci_scan_host_usb`), et les appels `loci_sync_kbd_from_sdl` dans la boucle d'événements SDL |

Après l'Epic, **`main.c` ne contient plus aucune *définition* de fonction
`loci_*`** — uniquement des appels.

## 5. L'extraction (US1 → US5)

Menée en lots à risque croissant, chacun vérifié byte-identique.

- **US1 — cartographie + oracle.** Analyse par bornes de fonction : un **seul**
  bloqueur (voir §6), une **seule** fonction SDL, seam propre. Oracle de
  déterminisme établi (§7). Cf. `loci-glue-carto.md`.
- **US2 — callbacks bus/IRQ.** `loci_dsk_cpu_irq_set/_clr`,
  `loci_dsk_sync_overlay`, `loci_rom_poke_hook`.
- **US3 — ROM/tape/resume.** 7 fonctions + déblocage `get_rom_patches` (§6).
- **US4 — USB + IRQ-trap.** `loci_attach_usb_dir`, `loci_scan_host_usb`,
  `loci_action_install/release_irq_trap`.
- **US5 — glue SDL.** `loci_sync_kbd_from_sdl` sous `#ifdef HAS_SDL2`.

## 6. Les deux points de découplage

**`get_rom_patches` (US3).** `loci_rom_swap_cb` re-sélectionne les patches ROM
après un swap via `get_rom_patches`, jusque-là static dans `main.c`. Comme
`loci_glue.c` fait partie de `LIB_OBJECTS` (linké par des binaires de test **sans
`main.o`**), une simple déclaration ne suffit pas : la **définition** doit vivre
en LIB. Les tables `rom_patches_basic10/11` + `detect_rom_version` +
`get_rom_patches` ont donc été extraites vers un nouveau module
`src/rom_patches.{c,h}`. `include/emulator.h` **n'a pas été modifié** (la
déclaration vit dans `rom_patches.h`) → aucun impact sur les projets qui
embarquent le header de l'émulateur.

**La couche SDL (US5).** Une seule fonction (`loci_sync_kbd_from_sdl`) dépend de
SDL. Elle est isolée sous `#ifdef HAS_SDL2` avec l'`#include <SDL2/SDL.h>` **à
l'intérieur** du garde, si bien que **`loci_glue.c` compile aussi en `SDL2=0`**.

## 7. L'oracle de non-régression

Le boot LOCI headless est **byte-déterministe**. Test `test-loci-golden`
(`tests/integration/test_loci_golden.sh`, dans `make tests`) : deux boots →
dumps RAM byte-identiques + écran non vide. Référence figée pour comparer
avant/après chaque lot :

```
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --headless -c 40000000 --dump-ram-at 39000000:REF.bin
# md5(REF.bin) = bf4dff781e05175ad8050815eccd6a42
```

Ce md5 est resté **inchangé** de US1 à US5 — la preuve que l'extraction n'a rien
altéré. (Distinct de `test-loci-e2e`, un test *comparatif* LOCI vs Microdisc
natif avec un écart de rendu Sedoric préexistant, hors `make tests`.)

## 8. Bilan

- `main.c` : **4988 → 3744 lignes** sur l'ensemble du chantier de dégonflement
  (dont -~400 l pour la glue LOCI).
- Nouveaux modules : `src/io/loci_glue.{c,h}`, `src/rom_patches.{c,h}`.
- Builds `SDL2=0`, `SDL2=1`, `SDL2=1 HTTPAPI=1` OK ; suite complète verte ;
  binaire byte-identique → aucun impact sur les projets utilisant l'émulateur.

## 9. Références

- `docs/architecture/loci-glue-carto.md` — cartographie US1 (tableau des 16 fonctions).
- `docs/architecture/io-bus.md` — l'adaptateur jumeau (`io_device_t`).
- `include/io/loci_glue.h`, `src/io/loci_glue.c` — l'adaptateur.
- `tests/integration/test_loci_golden.sh` — l'oracle.
