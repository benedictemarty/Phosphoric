# Base de temps sous-cycle du bus d'extension (modèle PHI2) — Épic B

- **Statut** : architecture (v2.0, 2.23.0) — chronologie en ns ; Phase 1 livrée
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
setup). Un périphérique **lent** — typiquement le LOCI, dont le RP2040 lit l'adresse
par PIO, la traite en logiciel (`act_loop`) puis pose la donnée par PIO — peut
**manquer** ce latch (le port d'extension n'a pas de RDY). Le VIA étant décodé-inhibé (cf. `io-bus.md`), **rien ne pilote
alors le bus** → le 6502 latche l'open-bus. À l'échelle du cycle entier, ce
phénomène est **invisible** : la lecture 6502 et le serve tombent « dans le même
cycle ». Il faut donc une base de temps **sous-cycle** pour le reproduire.

## 2. Modèle : chronologie en ns (2.23.0)

> **Correction 2.23.0.** La v1 divisait la période PHI2 **de l'Oric** en 30
> « subticks » en lisant `sys_clk = PHI2×30` (`cpu.c:158`). Or le Φ2 de `cpu.c` est
> un **réglage du firmware**, 4000 kHz par défaut (configuration vide) : le PIO
> tourne à 120 MHz, 1 tick = 8,33 ns, quatre fois plus fin. Avec la v1, une vraie
> LOCI aurait raté toutes ses lectures. Le « budget 26/36 cycles » du rapport de bug
> était en plus pris pour des subticks.

Origine : front descendant de PHI2 qui ouvre le cycle (`bus_timing.h`,
`bus_loci_read_valid_ps`, en picosecondes) :

| Étape | Instant | Source |
|---|---|---|
| mot d'action dans la FIFO | (22 + tior) ticks + 2 cycles sys | `mia.pio` (estimé) |
| donnée prête (DMA + IRQ 5) | + poll (0, non mesuré) + `serve` cycles sys | SysTick (`--loci-hw`) |
| donnée sur le bus | max(prête, montée de PHI2 + 2 cycles) + (3 + tiod) ticks | `mia_io_read` (estimé) |
| échéance du 6502 | période − tDSR (100 ns) | fiche 6502 à 1 MHz |

PHI2 de l'Oric : haut le **dernier tiers** du cycle (l'ULA à 12 MHz découpe le cycle
en 3 créneaux de 4 cycles, deux pour la vidéo, un pour le processeur ; forum Defence
Force t=2583). Avec les défauts : donnée à 708 ns tant que serve ≤ 58 cycles (elle
attend la montée de PHI2), échéance 900 ns, frontière à **81/82 cycles** de serve.
Mesure sur matériel (Feather 5723, `--loci-hw`) : serve 23 cycles → marge ≈ 190 ns.

Les périphériques **on-board** (RAM/ROM/VIA/ULA) ne passent pas par ce modèle : ils
sont toujours à temps, **aucun impact** sur l'existant.

### Client LOCI (`loci_mia_io_reliable`)

Deux modèles exclusifs de fiabilité du serve MIA :

- **WINDOW** (défaut, historique) : fiable ssi `tior ∈ [lo,hi]`. C'est la
  **calibration par carte** (le firmware `adj_scan` balaie tior 0-31 pour trouver
  la plage qui marche). Iso-comportement ; `--loci-mia-window LO-HI`.
- **PHASE** (opt-in) : chronologie ci-dessus, `--loci-serve-timing SERVE[,TDSR]`
  (SERVE en cycles du cœur 1, TDSR en ns) et `--loci-serve-jitter AMP[,SEED]`
  (± AMP cycles, seedé). `tior` et `tiod` (`MAP_TUNE_*`) entrent dans le calcul.
  Les durées réalistes (23 mesurés, 26/36 de l'analyse) sont **toutes propres** :
  la v1 « reproduisait » le rapport de bug `-Os`/`-O2` avec une grille fausse, et
  l'auteur du firmware attribue ce bug à un défaut de *mapping* d'adresse, pas au
  timing (`read-serve-et-inhibition-via.md`, correction du 2026-09-01).

`loci_set_mia_window()` bascule sur WINDOW, `loci_set_serve_timing()` sur PHASE.
Défaut au reset : WINDOW `[0,31]` → tout tior fiable.

## 3. Ce que la Phase 1 ne fait pas (encore)

- **Pas de réécriture sous-cycle du 6502.** `cpu_step` exécute une instruction
  entière ; le modèle de phase vit au **point de décision de l'accès bus** (lecture
  mémoire → périphérique io), là où la course compte. Une intégration sous-cycle
  profonde du CPU (chaque accès = un cycle bus horodaté en phase) est une phase
  ultérieure.
- **Un seul client** (LOCI, émulé et `--loci-hw`). Les autres périphériques du
  port d'extension auraient leur propre chronologie.

## 4. Feuille de route (épic B)

- [x] **Phase 1** — socle `bus_timing.h` + client LOCI (modèle PHASE opt-in, CLI
      `--loci-serve-timing`) + tests ; jitter seedé. Iso-comportement par défaut.
      2.23.0 : chronologie en ns partagée avec `--loci-hw`.
- [ ] **Phase 2** — autres périphériques du port d'extension.
- [ ] **Phase 3** — horodatage sous-cycle des accès au niveau CPU (chaque accès
      porte sa phase) ; setup/hold on-board si un cas réel l'exige.
- [ ] **Phase 4** — calibration contre un bus réel : comptes PIO, poll d'act_loop,
      tDSR du 6502 à 2 MHz (serve et act sont déjà mesurés par `--loci-hw`).

## 5. Références

- `include/io/bus_timing.h` — chronologie, échéance, jitter.
- `src/io/loci_boot.c` — `loci_mia_io_reliable`, `loci_set_serve_timing`.
- `src/io/io_bus.c` — application à l'ACIA `$0380` (open-bus + lecture destructive).
- `~/loci/extensions/analyse/read-serve-et-inhibition-via.md` — hypothèse
  `-Os`/`-O2` (caduque, cause = mapping d'adresse).
- `src/io/loci_hw.c` — mesures serve/act sur matériel réel ; `docs/loci.md`.
- `docs/architecture/io-bus.md` — dispatch page 3, inhibition VIA.
