# Phosphoric V2 — Précision au cycle réelle

**Statut** : plan approuvé pour exécution · **Créé le** : 2026-09-10
**Base** : 1.120.0-alpha · **Cible** : 2.0.0
**Méthode** : Scrum, sprints de 2 semaines, branche longue `v2/cycle-accuracy`
**Prérequis de lecture** : [docs/ACCURACY.md](../ACCURACY.md) (échelle N1→N4, classement actuel)

---

## 1. Pourquoi une V2

Le projet a communiqué « cycle-accurate ». L'audit du 2026-09-10 établit que le
cœur est **N2** (ordonné au cycle bus) et que la vidéo, le PSG et le FDC sont
**N1**. L'écart n'est pas cosmétique : il rend inatteignables les effets qui
font la valeur d'un émulateur Oric de référence (splits raster en milieu de
ligne, attributs commutés en cours de balayage, lecteurs de samples PSG
cadencés au timer, chargements bande au signal). Le corriger touche
l'ordonnancement de **tous** les composants : c'est une rupture d'architecture,
donc une V2 et non une série de correctifs 1.x.

**Objectif de la V2** : porter CPU, VIA, ULA et PSG au niveau **N3**
(pas-à-pas au cycle, en verrou), et le prouver par un harnais d'oracle externe
plutôt que par affirmation.

**Non-objectifs** (restent hors périmètre V2) : Telestrat, N4 généralisé
(seul le bus d'extension reste sous-cycle, via l'Épic B existant), modèle MFM
bit-à-bit généralisé du FDC (traité en option, pas en défaut).

---

## 2. Architecture cible

### 2.1 Horloge maître

Aujourd'hui le CPU est le maître du temps : `cpu_tick()` pousse un rappel
`on_cycle` par accès bus, puis `cpu_step()` **bourre** les cycles internes en
un bloc unique en fin d'instruction, et `cpu_cycle_tick()` propage ce bloc aux
périphériques. Conséquence : les cycles internes sont invisibles au bus, et
l'IFR du VIA peut être posé jusqu'à 6 cycles trop tard à l'intérieur d'une
instruction.

Cible : une fonction unique

```c
void emu_cycle(emulator_t* emu);   /* fait avancer TOUTE la machine d'un cycle */
```

avec un ordre intra-cycle figé et documenté :

1. **φ1** — l'ULA fait son accès mémoire (fetch caractère / motif / attribut).
2. **φ2** — le CPU exécute *son* unique action de cycle (accès bus réel,
   accès factice, ou cycle interne explicite).
3. **fin de cycle** — VIA, ACIA, FDC, PSG, DTL, Mageco évaluent leurs fronts
   sur exactement 1 cycle ; les lignes d'interruption sont mises à jour.
4. **échantillonnage** — l'état des lignes IRQ/NMI est latché pour la décision
   du CPU au cycle suivant (modèle pénultième).

`cpu_step()` devient un simple `while (!cpu_instruction_done) emu_cycle()`,
conservé pour le débogueur, les tests et l'API existante.

### 2.2 Cœur 6502 micro-séquencé

Chaque opcode devient une **séquence de micro-opérations** (une par cycle),
soit par table de micro-ops, soit par machine à états par mode d'adressage.
Chaque cycle porte : type d'accès (lecture / écriture / interne), adresse
émise, effet sur les registres. Les accès factices deviennent des cycles de
première classe et sont donc **visibles des périphériques**, ce qui est la
définition même de N3.

### 2.3 Compatibilité

- Les totaux de cycles par opcode ne changent pas → les 1001 tests existants
  restent la contrainte de non-régression n° 1.
- Le format `.ost` (savestate) évolue : la reprise doit pouvoir tomber **au
  milieu d'une instruction**. Nouvelle section `CPUµ` (index de micro-op,
  registres latents, latches d'interruption), version de format bumpée, lecture
  des `.ost` 1.x conservée (reprise en frontière d'instruction).

---

## 3. Épics

| # | Épic | Priorité | Sprints | Version |
|---|------|----------|---------|---------|
| **V2-E0** | Vérité de la communication + harnais d'oracle | Critique | S0-S1 | 1.121.0 |
| **V2-E1** | Cœur 6502 micro-séquencé (N3) | Critique | S2-S4 | 2.0.0-alpha.1 |
| **V2-E2** | Horloge maître & ordonnancement | Critique | S4-S5 | 2.0.0-alpha.2 |
| **V2-E3** | VIA 6522 au cycle | Haute | S5-S6 | 2.0.0-alpha.3 |
| **V2-E4** | ULA vidéo au cycle (fetch octet par cycle) | Haute | S6-S8 | 2.0.0-alpha.4 |
| **V2-E5** | PSG AY-3-8910 à `horloge/16` | Haute | S8-S9 | 2.0.0-alpha.5 |
| **V2-E6** | Cassette & FDC dérivés du temps réel | Moyenne | S9-S10 | 2.0.0-beta.1 |
| **V2-E7** | Perf, savestate, non-régression, CI | Critique | transverse | 2.0.0-rc |
| **V2-E8** | Documentation, communication, release | Haute | S11 | 2.0.0 |

---

### V2-E0 — Vérité de la communication + harnais d'oracle

Rien n'est modifié dans le cœur tant que l'oracle n'existe pas : c'est lui qui
transforme « je crois que c'est juste » en « c'est mesuré ».

- **US0.1 — Reformulation des allégations.** `README.md`, `CLAUDE.md`,
  `ROADMAP`, `docs/AGILE_PLAN.md`, en-têtes `cpu6502.h`, pages de distribution,
  notes de forum : appliquer le vocabulaire autorisé de `docs/ACCURACY.md`.
  Retirer la coche « Cycle-accurate timing » des *Success Metrics* et la
  remplacer par « N2 ordonné au cycle bus (N3 visé en 2.0.0) ».
  *Acceptation* : `grep -ri "cycle.accurate"` ne renvoie plus que des occurrences
  qualifiées ; un test de garde (`test_docs_claims.sh`) échoue si une occurrence
  nue réapparaît.
- **US0.2 — Oracle CPU externe. ✅ livré (v1.122.0-alpha)** — vecteurs
  **SingleStepTests/65x02** (10 000 cas par opcode, chacun avec l'état initial,
  l'état final et la trace bus attendue cycle par cycle), récupérés hors dépôt
  par `tools/fetch_vectors.sh` / `make fetch-vectors` (~1 Go, non versionnés).
  Exécuteur `tests/unit/test_cpu_cycles.c` → `make test-cycle` : parseur JSON
  maison (aucune dépendance), machine d'essai **64 Ko plats** (`rom_enabled=0`
  + callbacks d'I/O triviaux pour que `$0300-$03FF` et `$C000-$FFFF` ne soient
  ni avalés par le bus I/O ni en lecture seule), et **quatre propriétés mesurées
  séparément** : état final, total de cycles, **sous-séquence** bus (= la
  propriété N2), **séquence bus exacte** (= la propriété N3). SKIP propre sans
  vecteurs, et le parseur reste testé sur un cas embarqué. Les 12 opcodes JAM
  sont comptés à part (divergence de modélisation, hors échelle : le vrai NMOS
  bloque le bus, Phosphoric arrête le CPU).
  *Acceptation* : tenue — score publié, et socle `BUS_EXACT_FLOOR_BP` verrouillé
  contre les régressions, à faire monter par V2-E1.
- **US0.3 — Test fonctionnel Klaus Dormann. ✅ livré (v1.122.0-alpha)** —
  `tests/unit/test_dormann.c` → `make test-dormann` : `6502_functional_test.bin`
  chargé en 64 Ko plats, démarré en `$0400`, exécuté jusqu'au `jmp *`. **Il passe
  intégralement** (piège de succès `$3469`, ~96 M cycles émulés en 0,7 s de temps
  hôte) — le cœur est donc fonctionnellement sain, son déficit est bien temporel
  et non logique. Le test décimal n'est publié qu'en source `.a65` (aucun binaire
  en amont) : l'assembler exigerait `as65`, hors périmètre — le test fonctionnel
  couvre déjà le mode décimal.
- **US0.4 — Trace bus. ✅ livré (v1.122.0-alpha)** — nouveau crochet
  `cpu_set_bus_callback()` (une notification par accès bus réel, partagée avec
  l'oracle), module `src/utils/cycle_trace.c`, options `--cycle-trace FICHIER`
  et `--cycle-trace-max N`. Une ligne par cycle : `cycle, type (R/W/i), adresse,
  donnée, PC, A X Y SP P, drapeaux, ligne d'IRQ`. Les lignes `i` sont les cycles
  internes bourrés du N2 (sans adresse) ; elles porteront leur accès réel après
  V2-E1 — la trace rend donc le déficit **visible à l'œil** dès aujourd'hui.

### V2-E1 — Cœur 6502 micro-séquencé

- **US1.1 — Squelette micro-séquenceur. ✅ livré (v1.123.0-alpha)** —
  `src/cpu/microseq.c` : chaque instruction est décomposée en un **plan de
  micro-opérations, une par cycle** ; `cpu_cycle()` en exécute exactement une et
  `cpu_step()` n'est plus qu'un enrouleur. Les 256 opcodes sont couverts, classés
  en 17 familles de séquence ; le MODE d'adressage vient d'`opcode_table` (source
  unique) et la SÉMANTIQUE des fonctions partagées d'`opcodes.c`
  (`cpu_rmw_apply`, `cpu_op_adc/sbc/cmp/lax`, `cpu_sh_unstable`, `cpu_update_nz`)
  — les deux moteurs ne diffèrent **que** par l'ordonnancement, jamais par le
  calcul. Moteur **opt-in** (`--cpu-microseq`) le temps de la migration.
  *Acceptation tenue* : totaux de cycles identiques (Dormann réussit au même
  cycle près : 96 241 367 sur les deux moteurs), `make tests` inchangé.
- **US1.2 — Accès factices. ✅ livré (v1.123.0-alpha)** — tous présents dans les
  plans : lecture à l'adresse non corrigée (en traversée de page pour les
  lectures, **systématique** pour écritures et RMW), lecture de la base avant
  index en `zp,X`/`zp,Y` et `(zp,X)`, écriture-retour RMW, lectures de pile
  mortes (`PLA`/`PLP`/`JSR`/`RTS`/`RTI`), lecture morte des implicites, re-fetch
  de branche et cycle de correction de page. *Acceptation dépassée* :
  **100,00 %** de séquence bus exacte sur **2 440 000 cas** (l'objectif était
  « ≥ 99,9 % »), plus des tests unitaires qui montrent l'accès factice observé
  sur le bus et l'absence de ce même accès sur le moteur historique (`test-cpu`).
- **US1.3 — Interruptions au bon cycle. ✅ livré (v1.124.0-alpha)** —
  échantillonnage de /IRQ et /NMI **à chaque cycle**, la décision de fin
  d'instruction se fondant sur l'échantillon du **cycle pénultième**
  (`ms_irq_sampled`/`ms_nmi_sampled`). Le masque I est pris en compte **au moment
  de l'échantillonnage**, ce qui produit « gratuitement » la sémantique retardée
  de `CLI`/`SEI`/`PLP` (ils modifient I à leur dernier cycle). Latch de front NMI
  conservé ; **détournement NMI** si /NMI tombe avant le cycle qui empile P d'un
  `BRK` ou d'une séquence d'IRQ. *Acceptation tenue* : 7 tests unitaires dédiés
  (IRQ armée au dernier cycle vue trop tard + sa contre-épreuve, `SEI` qui ne
  protège pas, `CLI` et `PLP` qui retardent, détournement NMI/BRK, séquence
  d'interruption = 7 cycles tous porteurs d'un accès) ; boots ORIC-1/Atmos et
  **13 programmes du corpus** (6 disquettes + 7 cassettes) identiques.
  *A corrigé au passage* un défaut introduit en V2-S2 : la séquence
  d'interruption durait 8 cycles au lieu de 7 (un `M_DUMMY_PC` de trop) — invisible
  à l'écran, mais faux ; trouvé par le test « 7 cycles ».
- **US1.4 — Bourrage supprimé, moteur par défaut. ✅ livré (v1.124.0-alpha)** —
  le micro-séquenceur devient le moteur **par défaut** (`cpu_init`), donc plus
  aucun cycle bourré sur le chemin normal : tout cycle porte son accès. Le moteur
  historique reste disponible par `--cpu-legacy` (et `--cpu-microseq` est conservé
  en no-op pour les scripts existants). *Acceptation tenue* : `make tests`
  intégralement vert avec le nouveau défaut.

### V2-E2 — Horloge maître & ordonnancement

- **US2.1 — `emu_cycle()`. ✅ livré (2.0.0-alpha.1)** — `src/emu_clock.c` :
  point d'entrée unique du temps, avec l'ordre intra-cycle **figé et documenté**
  (φ1 ULA → φ2 CPU → périphériques φ2). Les compteurs de balayage
  (`raster_cycle`, `raster_rendered`, `raster_ng_line`, `raster_next_line`),
  jusque-là variables locales de la boucle principale, vivent désormais dans
  `emulator_t` : n'importe quel appelant peut cadencer la machine.
  `emu_step()` enroule `emu_cycle()` ; `emu_raster_pos()` donne la position du
  faisceau (ligne PAL 0-311, cycle dans la ligne 0-63) — la base du fetch par
  cycle de l'épic E4. Contrat complet dans
  [docs/architecture/master-clock.md](../architecture/master-clock.md).
- **US2.2 — Suppression des paquets. ✅ livré (acquis par US1.4, vérifié ici)** —
  avec le cœur micro-séquencé, chaque cycle est un accès bus : le rappel
  d'horloge reçoit **toujours `cycles = 1`**, donc `via_update`, `io_bus_tick`,
  `fdc_ticktock` et `cassette_tick` avancent d'un cycle à la fois. Mesuré :
  40 appels pour 40 cycles, maximum 1 cycle par appel
  (`test_peripherals_get_one_cycle_at_a_time`) — le test échouerait si un paquet
  réapparaissait.
- **US2.3 — Ordre intra-cycle documenté et testé. ✅ livré (2.0.0-alpha.1)** —
  `test_ula_reads_before_cpu_writes` monte le cas limite : une écriture du CPU
  tombant **exactement** au cycle où la scanline 0 est émise n'est pas visible
  dans cette ligne, mais l'est dans la suivante. C'est la convention de
  visibilité du matériel (l'ULA accède à la RAM en φ1, le CPU en φ2).
  Nouvelle suite `make test-clock` (8 tests).
- **US2.4 — Jonction Épic B. ✅ livré (documentaire)** — les 30 sous-ticks φ2 du
  bus d'extension (`include/io/bus_timing.h`) sont désormais explicitement
  décrits comme une **subdivision de la phase φ2** de `emu_cycle()` ; les
  périphériques de la carte mère gagnent toujours la course, leur coût reste nul.

### V2-E3 — VIA 6522 au cycle

- **US3.1 — Pas d'un cycle** pour T1/T2, registre à décalage, latches.
- **US3.2 — Timings d'arête exacts** : instant de pose de l'IFR après
  sous-dépassement, valeur lue du compteur pendant le cycle de rechargement,
  bascule PB7, N+2 / N+1,5 selon mode, handshakes CA1/CA2/CB1/CB2.
- **US3.3 — Vecteurs de timing** dérivés des chronogrammes de la datasheet
  (nouvelle section dans `docs/HARDWARE_CONFORMANCE.md`), plus un test
  d'intégration « IRQ timer 1 en mode continu à la période trame » qui vérifie
  la stabilité du décompte sur 50 trames.
- **US3.4 — Reprise des déviations assumées** listées en §2 du document de
  conformité, réévaluées à la lumière de N3.

### V2-E4 — ULA vidéo au cycle

Le gain le plus visible pour l'utilisateur.

- **US4.1 — Modèle de balayage complet** : 312 lignes × 64 cycles, zones
  active / bordure / blanking horizontal et vertical, position raster exposée
  (`emu_raster_pos()`) au débogueur et aux points d'arrêt raster.
- **US4.2 — Fetch octet par cycle** : chaque cellule caractère est lue au cycle
  où le vrai ULA la lit ; les attributs série sont latchés à leur fetch. Une
  écriture CPU en milieu de ligne n'affecte **que** la partie non encore
  balayée. Remplace `video_render_scanline()` (conservé comme chemin de repli
  pour l'export d'images statiques).
- **US4.3 — Modes** : TEXT 40×28, HIRES 240×200, lignes de texte basses,
  inversion, clignotement (compteur trame), double hauteur, intégration
  ULA-NG (`ula_ng_scanline` recadencé sur le nouveau modèle).
- **US4.4 — Corpus de référence** : images PPM de référence par démo/jeu du
  corpus existant, plus au moins **une démo à split raster** ajoutée au corpus
  et validée à l'œil puis figée en référence.
  *Acceptation* : `make test-video` étendu, aucune régression sur les
  références actuelles hors changements expliqués et re-baselinés explicitement.

### V2-E5 — PSG AY-3-8910 à `horloge/16`

- **US5.1 — Cadencement matériel** : compteurs de ton à `1 MHz/16` (62,5 kHz),
  bruit sur LFSR 17 bits réel, enveloppe à `horloge/256`, toutes les formes
  d'onde des 16 valeurs de R13.
- **US5.2 — Rééchantillonnage** propre vers 44,1 kHz (intégration par
  fenêtre / décimation filtrée) au lieu de l'échantillonnage direct actuel.
- **US5.3 — Non-régression audio** : les WAV de référence existants sont
  re-baselinés, et on ajoute une vérification **spectrale** (fréquence
  fondamentale mesurée à ±0,5 % de la valeur théorique `1e6/(16·période)`)
  plutôt qu'une comparaison octet à octet fragile.
- **US5.4 — Digidrums** : le chemin horodaté existant (`ay_write_data_timed`)
  est branché sur l'horloge maître ; test avec un lecteur de samples du corpus.

### V2-E6 — Cassette & FDC dérivés du temps réel

- **US6.1 — Bande au signal par défaut** : `--tape-signal` devient le chemin
  nominal, le patch ROM (fast-load) restant explicite (`-f`).
- **US6.2 — FDC : délais dérivés de la rotation** — DRQ et INTRQ calculés
  depuis `rot_pos` et le débit MFM (≈ 1 octet / 32 µs à 250 kbit/s) au lieu des
  constantes forfaitaires ; LOST DATA devient possible et testé.
- **US6.3 — Piste MFM optionnelle** (`--fdc-mfm`) réutilisant le parseur
  existant, pour les protections sensibles au timing. Option, pas défaut.

### V2-E7 — Perf, savestate, non-régression (transverse)

- **US7.1 — Budget de performance** : `make bench` devient bloquant. Cible :
  temps CPU hôte par trame émulée **≤ 5 %** du budget 20 ms sur la machine de
  référence. Le passage N3 coûte typiquement ×2 à ×3 sur le cœur ; la marge
  actuelle (~1 %) l'absorbe, mais la mesure décide.
- **US7.2 — Savestate `.ost` v2** : section `CPUµ`, reprise en milieu
  d'instruction, lecture rétrocompatible des `.ost` 1.x.
- **US7.3 — Corpus de non-régression** : les 41 programmes du corpus de
  compatibilité rejoués (`--movie-replay` + captures d'écran horodatées) avant
  et après chaque épic.
- **US7.4 — CI** : `make test-cycle`, `make test-dormann`, `make bench`,
  builds `SDL2=0/1`, Valgrind.

### V2-E8 — Documentation, communication, release

- **US8.1** — `docs/ACCURACY.md` repassé en « N3 atteint », preuves à l'appui.
- **US8.2** — `docs/architecture/master-clock.md` (ordre intra-cycle, contrat
  `emu_cycle`), mise à jour de `CLAUDE.md`, `README.md`, guide utilisateur.
- **US8.3** — Note technique publique : ce qui change de visible (splits
  raster, audio, chargements bande), et la rectification assumée de l'annonce
  initiale.
- **US8.4** — Release 2.0.0, tags, binaires, pages de distribution.

---

## 4. Séquence des sprints

| Sprint | Contenu | Sortie |
|--------|---------|--------|
| **V2-S0** | US0.1 (vérité de la communication) + `docs/ACCURACY.md` + garde anti-récidive | 1.121.0-alpha |
| **V2-S1** | US0.2/0.3/0.4 — oracle 65x02, Dormann, `--cycle-trace`, **score de base publié** | 1.122.0-alpha |
| **V2-S2** | US1.1 + US1.2 — micro-séquenceur **et** accès factices : 100 % d'un coup (l'oracle a permis d'aller plus loin que prévu) | 1.123.0-alpha |
| **V2-S3** | US1.3 + US1.4 — interruptions au cycle pénultième, drapeau I retardé, détournement NMI/BRK, **bascule du moteur par défaut** → **Épic V2-E1 terminé** | 1.124.0-alpha |
| **V2-S4** | US2.1 + US2.2 + US2.3 + US2.4 — horloge maître, fin des paquets, ordre intra-cycle testé → **Épic V2-E2 terminé** | 2.0.0-alpha.1 |
| **V2-S5** | US3.1 à 3.4 — VIA 6522 : timings d'arête exacts | 2.0.0-alpha.2 |
| **V2-S6** | US3.2/3.3/3.4 + US4.1 | 2.0.0-alpha.3 |
| **V2-S7** | US4.2 — fetch octet par cycle | branche |
| **V2-S8** | US4.3/4.4 + US5.1 | 2.0.0-alpha.4 |
| **V2-S9** | US5.2/5.3/5.4 + US6.1 | 2.0.0-alpha.5 |
| **V2-S10** | US6.2/6.3 + US7.2 | 2.0.0-beta.1 |
| **V2-S11** | US7.1/7.3/7.4 + Épic E8 | 2.0.0 |

Jalon de bascule du vocabulaire : **atteint pour le CPU à la fin de V2-S3**
(« cœur CPU exact au cycle, vérifié contre l'oracle 65x02 ») ; **fin de V2-S8**
pour la machine entière.

---

## 5. Risques et parades

| Risque | Impact | Parade |
|--------|--------|--------|
| Régression silencieuse sur le corpus de jeux | Élevé | US7.3 systématique à chaque fin d'épic ; captures avant/après diffées. |
| Perte de performance | Moyen | US7.1 bloquant ; micro-séquenceur en table plate, pas en pointeurs de fonction par cycle. |
| Explosion du coût de l'ULA au cycle | Moyen | Rendu paresseux : ne recalculer une portion de ligne que si la mémoire écran ou l'état ULA a changé depuis le dernier fetch. |
| Références PPM/WAV à re-baseliner en masse | Moyen | Chaque re-baseline est un commit séparé, justifié, avec l'image avant/après en pièce jointe du CR de sprint. |
| Branche longue divergente de `main` | Moyen | Rebase hebdomadaire ; les épics E0 et E8 vivent directement sur `main`. |
| Attente utilisateur : « V2 = tout change » | Faible | Communication US8.3 : V2 change la **fidélité**, pas la CLI ; compatibilité `.ost` assurée. |

---

## 6. Definition of Done (par user story)

1. `make clean && make tests` — 100 % vert, nombre de tests en hausse ou justifié.
2. `make test-cycle` — score en hausse ou stable, jamais en baisse.
3. `make bench` — dans le budget US7.1.
4. CHANGELOG, VERSION_TRACKING, CIRRUS_OS, ROADMAP mis à jour.
5. Documentation du composant touché à jour (dont `docs/ACCURACY.md` si le
   niveau d'un composant change).
6. Commit conventionnel, poussé sur les 4 miroirs.
