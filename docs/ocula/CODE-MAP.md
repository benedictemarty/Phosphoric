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

## 2 ter. Extraction réalisée (étape 2 — renderers, sur cette branche)

`video_internal.h` élargi expose les 7 helpers de rendu (non-static :
`set_pixel`, `get_rgb`, `get_charset_byte`, `decode_attr`, `render_attr_block`,
`effective_chline`, `blink_phase_on` — sans collision : `set_pixel` de mcp40.c
est `static`, signature différente). **`render_80col_scanline` et
`render_exthires_scanline` sont désormais dans `ocula_video.c`** ; `video.c` ne
fait que les appeler.

**Bilan isolation VIDÉO** : tout le code OCULA vidéo vit dans `ocula_video.c`
(palette/bordure/registres + les 2 renderers). Restent dans `video.c`, par nature
mêlés à l'ULA-NG et au cœur : les **branches de dispatch** (`apply_profile_resolution`,
`palette_latch`, latch de début de trame, `video_render_scanline`) — ce sont des
*aiguillages*, pas du rendu OCULA. Refactor pur, comportement identique : suite
complète verte (0 échec).

**Modules OCULA dédiés** : `src/io/ocula_io.c`, `src/io/ocula_gpu.c`,
`src/video/ocula_video.c` — l'empreinte OCULA compilable est isolée en 3 unités.

## 3. Reste : build `OCULA=0` (retrait complet) — non fait

Un binaire **compilable sans OCULA** (`make OCULA=0`, comme `CAST=0`/`HTTPAPI=0`)
prouverait l'isolation totale. Il exige d'exclure les 3 modules OCULA **et** de
**gater par `#ifdef HAS_OCULA` les ~25 sites d'appel** dans les fichiers cœur
(`video.c` dispatch, `memory.c` `memory_ocula_*`, `main.c` `--ula`/câblage,
`savestate.c` sections OCB/OGP). **Tradeoff honnête** : ça encombre 4 fichiers
cœur de `#ifdef` (contre la lisibilité), pour un gain surtout démonstratif. À
décider avant de l'exécuter (risque + clutter). L'isolation *structurelle* (code
en modules dédiés) est, elle, **acquise**.

## 4. Plan d'extraction modulaire (référence)

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
