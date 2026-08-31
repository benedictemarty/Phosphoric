# Base de temps sous-cycle du bus d'extension (modèle PHI2) — Épic B

- **Statut** : architecture (v1.0) — Phase 1 livrée
- **Auteur** : bmarty
- **Modules** : `include/io/bus_timing.h`, `src/io/loci_boot.c` (client LOCI),
  `src/io/io_bus.c` (point de décision ACIA `$0380`)
- **Grounding** : `~/loci/extensions/analyse/read-serve-et-inhibition-via.md`,
  `robustesse-lien-6502.md` (firmware `sodiumlb/loci-firmware`, schéma LOCI 1.3).

## 1. Problème

Le 6502 de l'Oric et les périphériques du **port d'extension** partagent un bus
**asynchrone** cadencé par PHI2. Phosphoric modélise le temps à la granularité du
**cycle entier** : `cpu_tick()` avance l'horloge à chaque accès bus (`cpu->cycles`
est exact à chaque lecture), mais il n'existe **aucune notion de phase intra-PHI2**.

Or certains conflits sont **sous-cycle** : la donnée doit être **stable sur le bus
avant l'instant de latch** du 6502 (proche du front descendant de PHI2, après le
setup). Un périphérique **lent** — typiquement le LOCI, dont le RP2040 échantillonne
le bus par PIO à `sys_clk = PHI2×30` (`cpu.c:158`) puis pose la donnée — peut
**manquer** ce latch. Le VIA étant décodé-inhibé (cf. `io-bus.md`), **rien ne pilote
alors le bus** → le 6502 latche l'open-bus. À l'échelle du cycle entier, ce
phénomène est **invisible** : la lecture 6502 et le serve tombent « dans le même
cycle ». Il faut donc une base de temps **sous-cycle** pour le reproduire.

## 2. Modèle (Phase 1)

Grille : la période PHI2 est divisée en `BUS_PHI2_SUBTICKS = 30` (rapport
sys_clk/PHI2 du LOCI, indépendant de la fréquence PHI2 réelle → tout est en
**fractions de période**).

- Le 6502 **latche** la donnée au subtick `latch_subtick` (défaut 27 = fin de
  PHI2 haut moins le setup).
- Un périphérique rend sa donnée valide au subtick `valid_subtick`.
- Lecture **propre** ssi `valid_subtick ≤ latch_subtick` ; sinon **course perdue**
  (open-bus). Prédicat : `bus_serve_wins_race()` (`bus_timing.h`).

Les périphériques **on-board** (RAM/ROM/VIA/ULA) sont valides tôt
(`valid_subtick = 0`) → gagnent toujours → **aucun impact**. Seuls les périphériques
du port d'extension à serve lent peuvent perdre. C'est la réalisation « globale »
mais **à coût nul pour l'existant** : la couche est générale, mais on ne route pas
les accès on-board à travers elle (ils gagneraient toujours).

### Client LOCI (`loci_mia_io_reliable`)

Deux modèles exclusifs de fiabilité du serve MIA :

- **WINDOW** (défaut, historique) : fiable ssi `tior ∈ [lo,hi]`. C'est la
  **calibration par carte** (le firmware `adj_scan` balaie tior 0-31 pour trouver
  la plage qui marche). Iso-comportement ; `--loci-mia-window LO-HI`.
- **PHASE** (opt-in, physiquement fondé) : le serve arrive au subtick
  `tior + serve_subticks` ; propre ssi `≤ latch_subtick`. `--loci-serve-timing
  SERVE[,LATCH]`. Rend explicites deux facteurs que WINDOW cache :
  - le **budget de serve** (≈ le build firmware) : l'analyse mesure ~26 cyc M0+ en
    `-Os` (optimisé) vs ~36 en baseline. À `latch=27` : `serve=26 → propre`,
    `serve=36 → raté`. **Reproduit exactement le rapport de bug** (le rebuild `-Os`
    corrige la lecture `$0380`).
  - l'**indépendance à la fréquence PHI2** (grille en fractions de période).

`loci_set_mia_window()` bascule sur WINDOW, `loci_set_serve_timing()` sur PHASE.
Défaut au reset : WINDOW `[0,31]` → tout tior fiable.

## 3. Ce que la Phase 1 ne fait pas (encore)

- **Pas de réécriture sous-cycle du 6502.** `cpu_step` exécute une instruction
  entière ; le modèle de phase vit au **point de décision de l'accès bus** (lecture
  mémoire → périphérique io), là où la course compte. Une intégration sous-cycle
  profonde du CPU (chaque accès = un cycle bus horodaté en phase) est une phase
  ultérieure.
- **Pas de jitter.** La décision est déterministe (tests reproductibles). Un jitter
  seedé dans la bande marginale est une option future.
- **Un seul client** (LOCI). Les autres périphériques du port d'extension
  brancheraient le même prédicat via leur propre `valid_subtick`.

## 4. Feuille de route (épic B)

- [x] **Phase 1** — socle `bus_timing.h` (grille PHI2×30, latch, prédicat de
      course) + client LOCI (modèle PHASE opt-in, CLI `--loci-serve-timing`) +
      tests. Iso-comportement par défaut.
- [ ] **Phase 2** — brancher les autres périphériques du port d'extension sur le
      prédicat (valid_subtick propre à chacun) ; jitter seedé optionnel.
- [ ] **Phase 3** — horodatage sous-cycle des accès au niveau CPU (chaque accès
      porte sa phase) ; setup/hold on-board si un cas réel l'exige.
- [ ] **Phase 4** — calibration des constantes (latch, budgets de serve) contre
      matériel réel (les valeurs actuelles sont modélisées dans les plages de
      l'analyse, pas mesurées au picoseconde).

## 5. Références

- `include/io/bus_timing.h` — grille, prédicat.
- `src/io/loci_boot.c` — `loci_mia_io_reliable`, `loci_set_serve_timing`.
- `src/io/io_bus.c` — application à l'ACIA `$0380` (open-bus + lecture destructive).
- `~/loci/extensions/analyse/read-serve-et-inhibition-via.md` — serve 26-36 cyc,
  sys_clk = PHI2×30, `-Os` vs `-O2`.
- `docs/architecture/io-bus.md` — dispatch page 3, inhibition VIA.
