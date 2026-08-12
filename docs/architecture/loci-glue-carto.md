# Cartographie de la glue LOCI (Epic 9 / US1)

Livrable de l'US1 : préparer l'extraction des fonctions `loci_*` de `main.c`
vers un adaptateur dédié `src/io/loci_glue.c` (même rôle que `io_bus.c`). Ce
document recense les fonctions, leurs dépendances bloquantes, la couche SDL, et
fixe l'oracle de non-régression.

## 1. Inventaire des fonctions `loci_*` de `main.c`

Mesuré sur `src/main.c` (analyse par bornes de fonction + intersection des appels
avec les statics de `main.c`).

| Fonction | ~L | SDL | Statics `main.c` non-`loci_` appelés | Lot |
|----------|----|-----|--------------------------------------|-----|
| `loci_dsk_cpu_irq_set`        |  4 | — | — | US2 |
| `loci_dsk_cpu_irq_clr`        |  4 | — | — | US2 |
| `loci_dsk_sync_overlay`       |  5 | — | — | US2 |
| `loci_rom_poke_hook`          |  5 | — | — | US2 |
| `loci_resume_snapshot_path`   |  4 | — | — | US3 |
| `loci_find_rom_file`          | 16 | — | — | US3 |
| `loci_find_menu_rom`          |  4 | — | — | US3 |
| `loci_patch_rom_info`         | 20 | — | — | US3 |
| `loci_tape_mount_cb`          | 35 | — | — | US3 |
| `loci_rom_swap_cb`            | ~101 | — | **`get_rom_patches`** | US3 |
| `loci_resume_session_cb`      | 13 | — | — | US3 |
| `loci_attach_usb_dir`         | 23 | — | — | US4 |
| `loci_scan_host_usb`          | 22 | — | — | US4 |
| `loci_action_install_irq_trap`| 21 | — | — | US4 |
| `loci_action_release_irq_trap`| 45 | — | — | US4 |
| `loci_sync_kbd_from_sdl`      | 30 | **SDL** | — | US5 |

Les appels croisés `loci_*` → `loci_*` (ex. `loci_scan_host_usb` → `loci_attach_usb_dir`,
`loci_action_release_irq_trap` → `loci_rom_swap_cb`/`loci_find_*`) ne sont **pas**
des bloqueurs : ces fonctions migrent **toutes ensemble** dans `loci_glue.c`.

## 2. Le seul vrai bloqueur : `get_rom_patches`

**Résultat décisif** : sur les 16 fonctions, une **seule** appelle un static de
`main.c` qui n'est pas lui-même un `loci_*` : `loci_rom_swap_cb` appelle
**`get_rom_patches(oric_model_t)`** (sélecteur pur des tables `rom_patches_basic10/11`,
~7 lignes) pour re-sélectionner les patches ROM après un swap.

→ **Action de déblocage (US3)** : dé-staticifier `get_rom_patches` et le déclarer
dans un header partagé (les tables `rom_patches_basic*` restent static dans
`main.c` — seul le sélecteur est exposé). Aucun autre travail de découplage n'est
requis : le seam d'enregistrement (`loci_set_*_callback(&emu.loci, cb, &emu)` +
`void* ctx`) rend l'extraction mécaniquement identique à `tape_patches` /
`serial_transport_create`.

## 3. Couche SDL isolée (US5)

Une **seule** fonction dépend de SDL : `loci_sync_kbd_from_sdl`
(`SDL_GetKeyboardState`, `SDL_GetModState`, `SDL_SCANCODE_*`). Elle sera placée
sous `#ifdef HAS_SDL2` dans `loci_glue.c`, qui **doit builder en `SDL2=0`**.

## 4. Oracle de non-régression (golden)

Le test comparatif `test_loci_sedoric_e2e.sh` (`make test-loci-e2e`, **hors**
`make tests`) vérifie la *correction* LOCI **vs** Microdisc natif ; ses 5 échecs
« texte écran » sont un **écart de rendu Sedoric-via-LOCI préexistant et stable**
(non lié à ce refactor) — ce n'est **pas** le bon oracle ici.

Pour l'extraction de glue, l'oracle est **auto-référentiel** : un boot LOCI
headless est **byte-déterministe**. Nouveau test `test-loci-golden`
(`tests/integration/test_loci_golden.sh`, **dans `make tests`**) : deux boots
LOCI → dumps RAM byte-identiques + écran non vide.

Référence figée pour comparer avant/après chaque US (média
`roms/loci/locirom` + `loci_demo.img`) :

```
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --headless -c 40000000 --dump-ram-at 39000000:REF.bin
# md5(REF.bin) = bf4dff781e05175ad8050815eccd6a42   (binaire pré-Epic-9)
```

Procédure US2-US6 : après chaque lot, régénérer ce dump et vérifier `md5` inchangé.

## 5. Découpage confirmé

- **US2** (lot sûr, couplage borné) : `loci_dsk_cpu_irq_set/_clr`,
  `loci_dsk_sync_overlay`, `loci_rom_poke_hook`.
- **US3** (ROM/tape/resume) : `loci_tape_mount_cb`, `loci_rom_swap_cb`
  (+ exposer `get_rom_patches`), `loci_resume_session_cb`, `loci_patch_rom_info`,
  `loci_find_rom_file`/`_menu_rom`, `loci_resume_snapshot_path`.
- **US4** (USB + IRQ-trap) : `loci_attach_usb_dir`, `loci_scan_host_usb`,
  `loci_action_install_irq_trap`, `loci_action_release_irq_trap`.
- **US5** (SDL) : `loci_sync_kbd_from_sdl` sous `#ifdef HAS_SDL2`.
- **US6** : `main.c` ne garde que les `loci_set_*_callback(...)`.

**Conclusion US1** : l'Epic est faisable sans surprise — un seul point de
découplage (`get_rom_patches`), une seule fonction SDL, un oracle déterministe
en place. Aucun static de `main.c` bloquant au-delà de `get_rom_patches`.
