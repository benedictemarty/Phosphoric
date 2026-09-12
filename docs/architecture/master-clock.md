# Horloge maître — contrat de `emu_cycle()`

**Module** : `src/emu_clock.c` · **Déclaration** : `include/emulator.h`
**Livré en** : V2-S4 (2.0.0-alpha.1) · **Plan** : [V2, épic E2](../specs/V2_CYCLE_ACCURACY.md)

## Pourquoi

Avant ce module, le temps de la machine était réparti entre trois endroits :

1. le CPU avançait sa propre horloge (`cpu_tick`) ;
2. un rappel par cycle défini **dans `main.c`** (`cpu_cycle_tick`) faisait avancer
   les périphériques φ2 ;
3. **la boucle principale calculait elle-même la position du balayage**
   (`frame_cycles / 64`) pour décider quand émettre une scanline.

Conséquence : seule la boucle principale savait cadencer la machine. Le
débogueur, les tests, le replay et les outils ne pouvaient pas demander « avance
d'un cycle » — ils ne pouvaient qu'exécuter une instruction et espérer que le
reste suive.

## Le contrat

```c
bool emu_cycle(emulator_t* emu);   /* un cycle de TOUTE la machine */
int  emu_step (emulator_t* emu);   /* une instruction, via emu_cycle */
```

`emu_cycle()` renvoie `true` quand le cycle exécuté terminait une instruction.
L'ordre intra-cycle est **figé** :

| Ordre | Qui | Quoi |
|-------|-----|------|
| 1 | CPU | l'unique accès bus du cycle (lecture, écriture, ou accès factice du NMOS) — sur le matériel, c'est le premier événement après le front montant de l'horloge 1 MHz |
| 2 | périphériques φ2 | VIA, FDC, ACIA, DTL, Mageco, cassette — avancés d'exactement **un** cycle par le rappel d'horloge du CPU, juste après l'accès bus |
| 3 | ULA | fetch d'**une cellule de 6 pixels** (la colonne = le count horizontal de ce cycle), puis avancement du balayage, tick raster ULA-NG |

### Pourquoi le CPU avant l'ULA — c'est mesuré, pas choisi

Le vrai ULA de l'ORIC et le 6502 se partagent la DRAM en deux moitiés du même
cycle de 1 µs, sans jamais se la disputer (pas de vol de cycle à modéliser,
contrairement à un ZX Spectrum). **L'ordre des deux moitiés a été mesuré à
l'oscilloscope** par Mike Brown (*ORIC 1/ATMOS Unofficial ULA Guide* 1.02,
§ Control and Sequencing) : au front montant de l'horloge 1 MHz, le compteur
horizontal s'incrémente et le 6502 fait son accès (« *it is asserting CAS here
that performs the write* ») ; **ensuite** « *the ULA access cycle begins* » —
l'ULA fetche l'octet écran de ce même count pendant la moitié basse, plus longue
(d'où l'horloge 1 MHz asymétrique). Conséquence observable : une écriture du CPU
au cycle *c* est vue par la cellule *c*. Vérifié par
`test_cpu_write_visible_in_the_same_cell` (`make test-clock`), qui échoue sur
l'ordre inverse.

Jusqu'en 2.0.1, Phosphoric faisait l'inverse (ULA d'abord, écriture visible en
*c+1*) : chaque split raster tombait **une cellule (6 pixels) trop à droite**.

Le même guide fixe le **repère horizontal** : le compteur de l'ULA compte 0-63,
les colonnes 0-39 sont fetchées aux counts 0-39, le blanking occupe 40-63 et
l'impulsion de synchro les counts 49-52. « Colonne 0 au cycle 0 de la ligne »
n'est donc pas une convention de l'émulateur mais le compteur du chip ;
`--ula-fetch-offset` reste un outil d'expérimentation, sa valeur juste est 0.
Quant à la **phase absolue** (quel cycle CPU depuis le reset tombe sur le count 0
de la ligne 0) : les compteurs de l'ULA tournent librement et le reset du 6502
est asynchrone, donc aucun logiciel ne peut l'observer sur un ORIC non modifié —
seul le « VSYNC hack » (fil ULA → VIA), non émulé, la rendrait visible.

### Pas de paquets

Avec le cœur micro-séquencé (le défaut depuis la v1.124.0), chaque cycle est un
accès bus : le rappel d'horloge reçoit donc toujours `cycles = 1`, et les
périphériques φ2 sont avancés **un cycle à la fois**. C'est vérifié par
`test_peripherals_get_one_cycle_at_a_time`, qui échouerait si un paquet
réapparaissait.

### Jamais d'appel à vide

Le contrat a une réciproque : **chaque appel à `emu_cycle()` coûte exactement un
cycle au CPU**. Une micro-op qui rendrait la main sans accès bus ferait avancer
l'ULA d'un cycle que ni le CPU ni le VIA n'auraient vécu — le compteur du CPU
resterait juste, l'oracle 65x02 ne verrait rien, et pourtant l'image dériverait.
C'est arrivé (2.0.0-alpha.8) : la décision « branchement non pris » était prise
un cycle trop tard, à vide, soit ~410 cycles d'avance du balayage par trame sur
la ROM BASIC. Depuis, `test_branch_not_taken_costs_no_phantom_cycle` et
`test_raster_and_cpu_stay_in_step_over_a_frame` (compteur CPU **=** position
raster sur une trame entière) le verrouillent.

### Sous-cycle (φ2 subdivisé)

La phase φ2 est elle-même subdivisée en **30 sous-ticks** pour le bus
d'extension (épic B, `include/io/bus_timing.h`) : c'est là que se joue la course
entre la carte et le front PHI2 suivant. Voir
[phi2-bus-timing.md](phi2-bus-timing.md). Les périphériques de la carte mère
n'en ont pas besoin (ils gagnent toujours la course), le coût est donc nul pour
eux.

## État porté par l'émulateur

Ces compteurs étaient des variables locales de la boucle principale ; les porter
dans `emulator_t` est précisément ce qui permet à n'importe quel appelant de
cadencer la machine :

| Champ | Rôle |
|-------|------|
| `raster_cycle` | cycle courant dans la trame (0 … 19967) |
| `raster_rendered` | scanlines visibles déjà émises (0 … 224) |
| `raster_ng_line` | ligne ULA-NG déjà traitée (0 … 311) |
| `raster_next_line` | cycle du prochain franchissement de ligne (sortie rapide) |
| `frame_cycles` | copie de `raster_cycle`, exposée aux points d'arrêt raster |

`emu_raster_pos(emu, &line, &dot)` donne la position du faisceau : ligne PAL
(0-311) et cycle dans la ligne (0-63). C'est la base du fetch octet par cycle de
l'épic E4.

## Cadrage de trame

```c
emu_clock_frame_begin(emu);   /* remet le balayage à zéro */
while (...) emu_step(emu);    /* une trame de cycles */
emu_clock_frame_end(emu);     /* termine les lignes restantes */
```

`emu_clock_frame_end()` existe pour le cas où le CPU s'arrête en plein écran
(halt, point d'arrêt) : l'image affichée doit rester complète.

La boucle principale ne compte pas ses propres cycles : elle lit `raster_cycle`.
C'est ce qui permet la **reprise d'un savestate en pleine trame** (V2-E7) : la
section `CLK` du `.ost` restaure `raster_cycle` / `raster_rendered` /
`raster_ng_line` et arme `clock_resume_pending` ; `emu_clock_resume()` — appelé
par `emu_clock_frame_begin()` ou par la boucle si le chargement a eu lieu en
cours de trame — recale `raster_next_line`, reconstruit depuis la RAM les lignes
déjà balayées (le framebuffer n'est pas sauvé) et, en mode ULA au cycle, rejoue
les cellules déjà fetchées de la ligne en cours pour retrouver l'état série de
ligne. Un état sauvé après une fin de trame (`-c`, `--save-state`) repart
simplement à zéro. Vérifié par `make test-savestate-determinism` : mêmes arrêts
raster, même VIA, même RAM qu'un run ininterrompu.

### La boucle principale, par étapes

Depuis la 2.0.3, `emulator_run()` (src/main.c) ne fait plus que dérouler, pour
chaque trame, des **étapes nommées** — `run_frame_instructions` (la boucle par
instruction autour de `emu_step`), puis les hooks de fin de trame (LOCI, son
headless, fast-load, frappe automatique, présentation et événements SDL,
captures, enregistrement, pokes, cadence) — en partageant un `run_state_t`
(cycles exécutés, trames, horloges). L'ordre des étapes est **observable** (une
capture `--screenshot-at` voit l'écran après la frappe automatique de la même
trame, jamais avant) : il est celui de la boucle historique et ne doit pas être
réordonné sans raison mesurée.

## Cœur historique

`--cpu-legacy` exécute une instruction d'un bloc et ne sait pas s'arrêter entre
deux cycles. `emu_cycle()` y exécute alors l'instruction entière puis rattrape le
balayage du même nombre de cycles, et renvoie toujours `true`. Le résultat est
identique à l'ancienne boucle — vérifié par
`test_legacy_core_advances_by_instruction` et par l'égalité des captures d'écran
sur le corpus.

## Performance

L'horloge est appelée un million de fois par seconde émulée. Deux précautions :

- la division par 64 a été remplacée par une **sortie rapide** sur
  `raster_next_line` : 63 cycles sur 64 ne font que deux additions et un test ;
- le fetch par cycle (épic E4) n'a coûté que **+4 %** : le travail total est le
  même (40 cellules par ligne), seule sa répartition change.

Mesuré : **601 µs par trame émulée** sur la machine de référence, soit **3,0 %**
du budget de 20 ms — le plafond fixé par le plan est de 5 %.
