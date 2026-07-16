# AUDIT.md — ULA-NG : audit d'architecture Phosphoric (préalable §0)

> Réponses aux 5 questions du §0 de `ULA-NG-SPEC.md` + résolution des marqueurs
> `[À CONFIRMER]`. Toutes les citations sont `fichier:ligne` vérifiées sur le
> code au moment de l'audit (v1.65.0-alpha). Aucun code fonctionnel n'a été écrit.

## Verdict de faisabilité (résumé)

| Point spec | Résultat | Conséquence |
|---|---|---|
| Granularité vidéo | **scanline-based (b)** — déjà en place | **Pas de conversion du pipeline** : le prérequis §0 est rempli. Les features raster/palette-par-ligne sont directement branchables. |
| Fenêtre registres `$0340-$035F` | **LIBRE** (grep exhaustif : 0 occurrence) | Adoptée telle quelle, pas de décalage. |
| Point d'indirection palette (§5.1) | `get_rgb()` unique → `vid->pal_rgb[8][3]` | Un seul endroit à faire passer par la LUT NG. |
| Ligne d'IRQ (§5.2) | Bitfield level-triggered OU-câblé | Ajouter une source `IRQF_ULANG = 0x20` (prochain bit libre), combinée, pas écrasée. |
| Source de temps | `emu->frame_cycles` ; ligne = `frame_cycles / 64` | `ula_ng_scanline(line)` appelable depuis la boucle de rendu existante. |
| Précédent vidéo | OCULA (raster-sync `$03EC-$03ED`, palette redéfinissable) | Modèle d'intégration éprouvé à suivre. |

---

## 0.1 — Langage, build, tests

- **Langage** : C11 (`-std=c11`), `Makefile:7` (`-Wall -Wextra -Wpedantic`).
- **Build** : `make` (headless) / `make SDL2=1` (affichage) ; `CMakeLists.txt` en second.
- **Tests** : suite maison (macros `TEST()/RUN()/ASSERT_*`, pas de dépendance). Vidéo :
  `make test-video` (`tests/unit/test_video.c`). `make tests` = suite complète (987 tests).

## 0.2 — Granularité du rendu vidéo → **(b) scanline-based**

Le rendu est **scanline-accurate** : une ligne est composée à chaque ligne
balayée, à partir de la VRAM figée à l'instant CPU courant.

- Boucle par frame : `src/main.c` while `frame_cycles < CYCLES_PER_FRAME`
  (~`main.c:2272`). Après chaque `cpu_step`, `emu->frame_cycles` est mis à jour.
- Rattrapage scanline : `src/main.c:2389-2392`
  ```c
  int target_scanline = frame_cycles / PAL_CYCLES_PER_LINE;   /* 64 cyc/ligne */
  while (rendered_scanlines < target_scanline && rendered_scanlines < 224) {
      video_render_scanline(&emu->video, emu->memory.ram, rendered_scanlines);
      rendered_scanlines++;
  }
  ```
  (idem catch-up fin de frame `main.c:2397-2399`, et un chemin stall `2325-2327`).
- Fonction de rendu : `void video_render_scanline(video_t* vid, const uint8_t* memory, int y)`
  — `src/video/video.c:437` (proto `include/video/video.h:176`). Lit la VRAM
  (`base = hires ? 0xA000 + y*40 : 0xBB80 + row*40 ; byte = memory[base+col]`),
  compose 6 pixels/octet, écrit RGB888 via `set_pixel()`.
- **Latchs par ligne déjà présents** : `palette_latch()` (video.c:480) et
  `border_latch()` (video.c:484) sont relus **à chaque scanline** → crochet
  naturel pour la palette-par-ligne (§5.4) et l'IRQ raster (§5.2).

> **Conséquence spec** : la conversion frame→scanline exigée conditionnellement
> par le §0 **n'est pas nécessaire**. `ula_ng_scanline(line_number)` s'insère
> juste avant/à l'appel de `video_render_scanline` dans la boucle `main.c`.

## 0.3 — Interception des accès mémoire (page 3)

- Toute la page 3 `$0300-$03FF` est routée vers des callbacks :
  `src/memory/memory.c:209-216` (lecture) / `275-281` (écriture) →
  `mem->io_read/io_write`.
- Callbacks enregistrés dans `src/main.c:1577`
  (`memory_set_io_callbacks(..., io_read_callback, io_write_callback, emu)`).
- Dispatch **lecture** : `io_read_callback()` `src/main.c:919-996` ; **écriture** :
  `io_write_callback()` `src/main.c:1112-1227`. Cascade « premier match par
  adresse », **VIA `$0300-$030F` en fallback final** (miroir sur toute la page 3).
  Ordre : LOCI ($03A0-$03BF, $0315-$0319), OCULA ($03E0-$03EF), ACIA
  ($031C-$031F, base `--acia-addr`), Mageco, Microdisc ($0310-$031F), DTL2000
  ($03F8-$03FD), puis VIA.
- **Point d'insertion ULA-NG** : un test `if (ula_ng_addr_in_window(address))
  return ula_ng_read(...)` à ajouter **avant le fallback VIA** dans les deux
  dispatchs (symétrique à OCULA).

### 0.3b — Conflit d'adresses `$0340-$035F` → **AUCUN**

`grep -niE '0x034[0-9a-f]|0x035[0-9a-f]'` sur `src/` + `include/` : **0 occurrence**.
La fenêtre tombe entre Microdisc/LOCI ($0310-$031F) et OCULA ($03E0-$03EF), et
n'est touchée par aucune extension (y compris bases configurables). **Fenêtre
`$0340-$035F` adoptée sans décalage.**

## 0.4 — Structure de la couleur

- Palette « en dur » 8 couleurs RGB888 : `src/video/video.c:17-20`
  ```c
  static const uint8_t palette[8][3] = { {0,0,0},{FF,0,0},{0,FF,0},{FF,FF,0},
                                         {0,0,FF},{FF,0,FF},{0,FF,FF},{FF,FF,FF} };
  ```
- **Point de conversion unique** : `get_rgb()` `src/video/video.c:109-113` →
  `c = oric_color & 7 ; *r/g/b = vid->pal_rgb[c][...]`. La palette **active** est
  déjà une LUT `uint8_t pal_rgb[8][3]` (`include/video/video.h:149`), réinitialisée
  par `palette_reset()` (video.c:31-33) et éventuellement redéfinie par OCULA
  (`palette_latch()`, video.c:67-83, RGB332 → RGB888).

> **Conséquence spec (§5.1)** : `pal_rgb[8][3]` EST déjà la LUT demandée. La
> palette-indirection NG = étendre chaque entrée à 12 bits (4096 teintes) et
> alimenter `pal_rgb` (ou une variante RGB444→888) depuis les registres NG au
> lieu de la table fixe. Un seul point (`get_rgb`) reste à toucher.

## 0.5 — Horloge et IRQ

- **Source de temps accessible au code vidéo** : `emu->frame_cycles`
  (`include/emulator.h:215`, `int` 0-19967), mis à jour par instruction
  (`main.c` ~2343). Numéro de ligne = `frame_cycles / PAL_CYCLES_PER_LINE` (64).
  Constantes `PAL_LINES_PER_FRAME=312`, `CYCLES_PER_FRAME=19968` (emulator.h:106-111).
- **IRQ 6502 = bitfield level-triggered, OU-câblé** : `cpu.irq` (uint8_t) ;
  sources `cpu_irq_source_t` `include/cpu/cpu6502.h:44-48` :
  `IRQF_VIA=0x01, IRQF_DISK=0x02, IRQF_SERIAL=0x04, IRQF_DTL2000=0x08,
  IRQF_MAGECO=0x10`. **Prochain bit libre = `0x20`.**
- API : `cpu_irq_set(cpu, source)` / `cpu_irq_clear(cpu, source)`
  (cpu6502.h:161/172). L'IRQ est prise si `cpu.irq != 0` et I-flag clear.
  Chaque source maintient son bit indépendamment → **on combine, on n'écrase pas**
  (exigence §5.2 respectée par construction).
- Exemple de raccord existant (VIA) : `irq_callback()` `main.c:1237-1244`
  (`state ? cpu_irq_set(IRQF_VIA) : cpu_irq_clear(IRQF_VIA)`). Microdisc/ACIA/
  DTL2000/Mageco suivent le même patron (`main.c:1269-1320`).

> **Conséquence spec (§5.2)** : ajouter `IRQF_ULANG = 0x20` à l'enum, et
> asserter/acquitter via `cpu_irq_set/clear(&emu->cpu, IRQF_ULANG)` depuis la
> logique raster NG. Zéro impact sur les IRQ existantes.

---

## Plan d'intégration proposé (mirroir FPGA, §1.3)

Module isolé `src/io/ula_ng.c` + `include/io/ula_ng.h`, 3 interfaces :

1. `uint8_t ula_ng_read(ula_ng_t*, uint16_t addr)` / `void ula_ng_write(ula_ng_t*, uint16_t addr, uint8_t val)`
   — branchés dans `io_read_callback`/`io_write_callback` (main.c) **avant le
   fallback VIA**, gardés par `ula_ng_addr_in_window(addr)` (`$0340-$035F`).
2. `void ula_ng_scanline(ula_ng_t*, int line)` — appelé dans la boucle de rendu
   `main.c` au moment du rattrapage scanline (à côté de `video_render_scanline`).
   Y placer : test IRQ raster (`line == NG_RASTERLINE` → `NG_STATUS.b7` +
   `cpu_irq_set(IRQF_ULANG)`), application palette-par-ligne, latch start-address.
3. Sortie IRQ → `cpu_irq_set/clear(&emu->cpu, IRQF_ULANG)` (nouveau bit `0x20`).

État verrouillé (reset) : registres à 0, `ula_ng_read` inerte, `get_rgb`
inchangé → **bit-à-bit identique à l'actuel** (non-régression §7). Le déverrouillage
(`$0340` ← 'N','G') active seulement alors `NG_MODE.b0`.

### Ordre d'implémentation (chaque étape testée, cf §4 spec)
1. `NG_LOCK`/`NG_ID` + plomberie page 3 (module + dispatch + verrou).
2. Palette-indirection (§5.1) — brancher `pal_rgb` sur la LUT NG 12 bits.
3. IRQ raster (§5.2) — `IRQF_ULANG`, `ula_ng_scanline`.
4. Start-address (§5.3), 5. palette/scanline (§5.4), 6. scroll fin (§5.5),
   7. attributs parallèles (§5.6), 8. sprites (§5.7), 9. chunky/80col (§5.8).
5. Mode trace `--ula-trace=FILE` (§6) + `README-ULA-NG.md` (§8).

### Cible FPGA (décision figée, photo produit + wiki Sipeed)
**Sipeed Tang Primer 20K** — bundle **Core Board + Dock ext-board**, Gowin
**GW2A-LV18PG256C8/I7** : 20 736 LUT4, 15 552 FF, **BSRAM 828 Kbit** (46 blocs),
**DDR3 128 Mbit** + NOR flash 32 Mbit sur la carte cœur. Répartition mémoire :
- **DDR3 128 Mbit (carte cœur)** — pas de RAM in-package sur le GW2A :
  plan d'attributs parallèles (§5.6), banques VRAM, tables sprites — hors des 64 Ko 6502.
- **BSRAM ~828 Kbit (budget serré)** : line buffers de composition, LUT palette (16×12 b),
  charset, petits caches sprites — jamais de framebuffer complet.
- **Sortie vidéo HDMI disponible via le dock** (+ RGB565 FPC, Ethernet, USB-OTG/JTAG,
  GPIO) — cible de sortie du futur HDL ; sans impact sur le comportement de référence.

### Points « pensée FPGA » déjà favorables
- Pas de flottant dans le chemin vidéo (RGB888 entiers, palette LUT). ✓
- `pal_rgb`, registres NG = petites tables/registres → équivalent HDL direct. ✓
- Échantillonnage déterministe : les latchs sont **déjà par scanline**
  (palette_latch/border_latch) → registres NG lus « une fois par ligne ». ✓

> À documenter par registre (exigence §9) : lecture combinatoire (ligne courante)
> vs synchrone (ligne/trame suivante). Recommandation : effets appliqués **à la
> ligne suivante** (comme les attributs sériels actuels, cf video_render_scanline
> qui fige la VRAM à l'instant CPU) pour rester fidèle au balayage.
