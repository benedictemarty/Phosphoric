# OCULA — carte de code & plan d'isolation

> **Branche `feature/ocula-isolation`** (hors `main`). Objet : délimiter
> précisément le sous-système OCULA (remplacement d'ULA RP2350 de sodiumlb,
> testbed logiciel de Phosphoric) et poser un plan d'extraction modulaire.
> OCULA est **fonctionnellement complet** sur `main` (plan 5 étapes, sprints
> 64-73) mais son code est **tissé dans les fichiers partagés** — ce document
> l'isole *conceptuellement* avant toute extraction *physique*.

## 1. Empreinte du code (inventaire réel)

### Fichiers dédiés (déjà isolés)
| Fichier | Rôle |
|---|---|
| `include/io/ocula_io.h`, `src/io/ocula_io.c` | Fenêtre ID + banking `$03E0-$03E7`, fichier de registres write-only (palette/bordure), déverrouillage knock `'O','C'`. |
| `include/io/ocula_gpu.h`, `src/io/ocula_gpu.c` | OCULA-GPU : fenêtre de commandes `$03E8-$03EF` (INFO, FILL, COPY, SCROLL, WAIT_VBL). |
| `tests/unit/test_ocula.c` | ~101 tests OCULA (gating, palette, bordure, exthires, 80col, GPU, compositing). |
| `docs/ocula_extensions.md`, `docs/ocula_firmware_alignment.md` | Spec (v0.10) + alignement firmware `ocula-pivic-firmware` v0.1.4. |
| `demos/ocula/*` | 6 démos (.bas/.s) + menu + README. |

### Code dispersé dans les fichiers PARTAGÉS (≈ 378 lignes)
| Fichier | Lignes OCULA | Contenu |
|---|---|---|
| `src/video/video.c` | ~82 | `ocula_block_armed()`, `ocula_regs_active()`, `border_latch()`, `render_80col_scanline()`, `render_exthires_scanline()`, latches trame (`ocula_80col`/`exthires`), `video_compose_bordered()`, `apply_profile_resolution()`. |
| `include/video/video.h` | ~54 | Constantes `OCULA_*` (80COL, EXTHIRES, BANK, BORDER, GPU…), champs `video_t` (`ula_profile`, `ocula_*`, `ocula_border[]`, pointeurs registres). |
| `src/memory/memory.c` | ~85 | `memory_ocula_{page_mapped,page_read,page_write,reg_write,set_bank,get_bank,unlock_write,unlocked,regs_armed}`, `ocula_window_ptr()`. |
| `include/memory/memory.h` | ~62 | Constantes banking, prototypes `memory_ocula_*`, champs état. |
| `src/main.c` | ~54 | `--ula PROFILE`, câblage fenêtre ID `$03E0-$03E7`, GPU, mirroir registres par trame. |
| `src/savestate.c` | ~36 | Sections `.ost` **OCB** (état OCULA) et **OGP** (état GPU). |
| `src/video/renderer.c` | ~5 | Sortie composée bordée (overscan). |

## 2. Carte mémoire / registres OCULA

- **In-band** (RAM écran) : palette `$BFE0-$BFE7` + magic `'O','C'` `$BFE8/$BFE9`,
  bordure `$BFEA` ; modes via attributs série 25/27/29/31 (80col, ext-HIRES).
- **Registres page 3** (`--ula ocula`) : ID/banking `$03E0-$03E7`, GPU `$03E8-$03EF`.
- **Constantes** : `OCULA_80COL_*`, `OCULA_EXTHIRES_*`, `OCULA_BANK_*`,
  `OCULA_BORDER_*`, `OCULA_GPU_*`, `OCULA_CAP_*` (voir `video.h`/`ocula_gpu.h`).
- **Gating** : profil `--ula ocula` + déverrouillage opt-in (knock `'O','C'`) →
  inerte sinon (rendu HCS10017 bit-à-bit).

## 2 bis. Extraction réalisée (étape 1, sur cette branche)

**`src/video/ocula_video.c`** (+ `src/video/video_internal.h` pour les 4 helpers
partagés) reçoit le cluster OCULA **palette / bordure / registres**, sans aucune
dépendance aux helpers de rendu statiques de `video.c` :
`rgb332_to_rgb888`, `ocula_block_armed`, `ocula_regs_active`, `border_latch`,
`video_get_border_rgb`, `video_bordered_w/h`, `video_compose_bordered`.

`video.c` ne fait plus qu'appeler ces fonctions (via `video.h` / `video_internal.h`).
Ajout de `ocula_video.c` aux 6 cibles Makefile qui linkent `video.c`.
**Refactor pur, comportement identique** : suite complète verte (test-ocula 101,
test-video 14, test-renderer 10, test-savestate 13, 0 échec global).

**Reste dans `video.c`** (trop couplé aux helpers statiques `set_pixel`/`get_rgb`/
`decode_attr`/… — étape 2) : `render_80col_scanline`, `render_exthires_scanline`,
et les branches OCULA de `apply_profile_resolution`/`palette_latch`/le latch de
début de trame (mêlées à l'ULA-NG et au cœur).

## 3. Plan d'extraction modulaire (suite)

Objectif : ramener le reste du code dispersé derrière des interfaces nettes, pour
qu'OCULA soit compilable/désactivable comme un module (à l'image de l'ULA-NG,
déjà isolée dans `src/io/ula_ng.c` + pointeurs `video_t`).

1. **Vidéo** : extraire `render_80col_scanline`/`render_exthires_scanline`/
   `border_latch`/`ocula_block_armed`/`ocula_regs_active`/`video_compose_bordered`
   vers `src/video/ocula_video.c` ; `video.c` n'appelle que des hooks
   (`ocula_render_scanline(vid, …)` renvoyant « pris en charge » ou non), sur le
   modèle des pointeurs `ng_*` de l'ULA-NG.
2. **Mémoire** : `memory_ocula_*` sont déjà nommés — les déplacer vers
   `src/memory/ocula_bank.c` + un hook unique dans `memory.c` (`if (ocula_page_mapped) …`).
3. **Savestate** : sections OCB/OGP derrière `ocula_savestate_{write,read}()`.
4. **Flag de build** : `OCULA=0` compile un binaire sans OCULA (comme `CAST=0`),
   pour prouver l'isolation.
5. **Non-régression** : `make test-ocula` (101), `test-video`, `test-savestate`,
   `test-renderer` + garde visible — **la suite complète doit rester verte**.

## 4. Risques & réserve

- OCULA touche le **cœur rendu/mémoire/savestate** : l'extraction physique est un
  refactor **substantiel** à mener par petits pas, chacun validé par la suite.
- Ce document **isole/délimite** le sous-système (étape sûre). L'extraction
  physique (`OCULA=0` compilable) est le livrable suivant, sur cette branche.

## 5. Références

- `docs/ocula_extensions.md` (spec v0.10), `docs/ocula_firmware_alignment.md`.
- Sprints ROADMAP 64-73 (OCULA). Firmware : `ocula-pivic-firmware` v0.1.4.
- Modèle d'isolation cible : `src/io/ula_ng.c` (ULA-NG, déjà modulaire).
