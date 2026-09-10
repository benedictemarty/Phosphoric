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

- **US1.1 — Squelette micro-séquenceur.** `cpu_cycle()` + état de micro-op dans
  `cpu6502_t` ; `cpu_step()` réécrit par-dessus. Tous les opcodes migrés, à
  totaux de cycles inchangés. *Acceptation* : les 1001 tests passent inchangés.
- **US1.2 — Accès factices.** Page-cross indexé (lecture à l'adresse non
  corrigée), `zp,X`/`zp,Y` (lecture de la base avant l'index), `(zp,X)`,
  `(zp),Y`, RMW `abs,X` (toujours 7 cycles avec factice), stores `abs,X/Y`
  (toujours 5), re-fetch de branche sur page-cross, cycles factices de pile
  (`PHA`/`PLA`/`JSR`/`RTS`/`RTI`/`BRK`). *Acceptation* : score `test-cycle` sur
  les accès ≥ 99,9 % ; test dédié montrant qu'un factice sur un registre I/O à
  effet de bord est bien observé.
- **US1.3 — Interruptions au bon cycle.** Échantillonnage IRQ/NMI au cycle
  pénultième ; latch de front NMI ; détournement `BRK`→NMI ; sémantique
  retardée de `CLI`/`SEI`/`PLP` ; comportement IRQ pendant une branche.
  *Acceptation* : vecteurs dédiés + non-régression sur le boot ROM 1.0/1.1 et
  sur les jeux du corpus.
- **US1.4 — Bourrage supprimé.** Plus aucun `cpu_tick(n>1)` dans le cœur.
  *Acceptation* : garde de compilation/test (`grep` + assertion runtime).

### V2-E2 — Horloge maître & ordonnancement

- **US2.1 — `emu_cycle()`** et migration de la boucle principale, du débogueur,
  du mode headless, du replay et de l'API `--control`/HTTP sur ce point unique.
- **US2.2 — Suppression de `via_update(paquet)`** au profit d'un pas d'un
  cycle ; idem `io_bus_tick`, `fdc_ticktock`, `cassette_tick`.
- **US2.3 — Ordre intra-cycle documenté et testé** (φ1 ULA / φ2 CPU / fronts),
  avec un test qui vérifie qu'une écriture CPU dans la mémoire écran au cycle
  *c* est vue par l'ULA au cycle *c+1* et pas *c*.
- **US2.4 — Point de jonction Épic B** : les 30 sous-ticks φ2 du bus
  d'extension deviennent une subdivision de l'étape φ2 de `emu_cycle()`.

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
| **V2-S2** | US1.1 — micro-séquenceur, totaux inchangés | branche |
| **V2-S3** | US1.2 — accès factices | branche |
| **V2-S4** | US1.3/1.4 + US2.1 — interruptions au cycle, `emu_cycle()` | 2.0.0-alpha.1 |
| **V2-S5** | US2.2/2.3/2.4 + US3.1 | 2.0.0-alpha.2 |
| **V2-S6** | US3.2/3.3/3.4 + US4.1 | 2.0.0-alpha.3 |
| **V2-S7** | US4.2 — fetch octet par cycle | branche |
| **V2-S8** | US4.3/4.4 + US5.1 | 2.0.0-alpha.4 |
| **V2-S9** | US5.2/5.3/5.4 + US6.1 | 2.0.0-alpha.5 |
| **V2-S10** | US6.2/6.3 + US7.2 | 2.0.0-beta.1 |
| **V2-S11** | US7.1/7.3/7.4 + Épic E8 | 2.0.0 |

Jalon de bascule du vocabulaire : **fin de V2-S4** pour le CPU (« CPU exact au
cycle, vérifié contre 65x02 »), **fin de V2-S8** pour la machine entière.

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
