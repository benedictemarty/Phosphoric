# Architecture — Bus I/O & périphériques (`io_device_t`)

> **Branche `feature/io-device-bus`.** Sort de `main.c` la cascade de `if
> (has_X && X_addr_in_range(addr)) return X_read(...)` (le dispatch page 3 câblé
> en dur, ~25 périphériques) au profit d'une **table de périphériques**. But :
> ajouter/retirer un périphérique = enregistrer/retirer **une entrée**, sans
> toucher au cœur. Le retrait d'OCULA a montré le coût de l'absence de cette
> abstraction (code saupoudré dans main/memory/video/savestate).

## 1. Le problème

`main.c` (~4600 lignes) est un god-object : chaque périphérique y est recâblé à
la main en 5+ endroits (CLI, init, **dispatch I/O**, boucle principale,
savestate, glue). `io_read_callback`/`io_write_callback` sont deux cascades de
`if` avec priorités et **interdépendances croisées** (ACIA↔Microdisc, fenêtre
ORICON qui recouvre le Microdisc, fiabilité MIA du LOCI pour l'ACIA à $0380…).

## 2. Ce qui est un « périphérique de bus » (et ce qui ne l'est pas)

Critère : **revendique une plage d'adresses en page 3** (± ROM overlay).

- **Cœur** (NE PAS traiter comme périphérique) : 6502, mémoire, **VIA**, ULA,
  PSG, clavier. Soudés, c'est l'Oric.
- **Devices bus** (cible de `io_device_t`) : **Microdisc** ($0310-$031F),
  **ACIA 6551** ($031C-$031F), **ULA-NG** ($0340-$035F), **LOCI**
  ($03A0-$03BF + TAP/DSK), **DTL 2000** ($03F8-$03FD), **Mageco/ORICON**
  ($03FE-$03FF / $031C-$031E).
- **Port-attached** (périphériques, mais PAS des devices bus) : joystick
  (PSG Port A), imprimante/MCP-40 (VIA Port A + CA2), cassette (VIA CB1). Ils
  se pilotent via les callbacks de port existants — **hors** de cette abstraction.

## 3. Le contrat

`include/io/io_device.h` :

```c
typedef struct io_device_s {
    const char* name;
    bool    (*claims)(struct emulator_s* emu, uint16_t addr);        /* claim LECTURE (+ écriture par défaut) */
    uint8_t (*read)(struct emulator_s* emu, uint16_t addr);
    bool    (*write)(struct emulator_s* emu, uint16_t addr, uint8_t value); /* true = consommé, false = repli VIA */
    bool    (*claims_write)(struct emulator_s* emu, uint16_t addr);  /* optionnel (NULL → claims) */
    const char* save_tag;                                            /* section .ost (NULL → aucune) */
    bool    (*save)(struct emulator_s* emu, FILE* fp);               /* false → pas de section */
    void    (*load)(struct emulator_s* emu, FILE* fp, uint32_t size);
    uint8_t (*peek)(struct emulator_s* emu, uint16_t addr);          /* lecture sans effet de bord */
    size_t  present_off;                                             /* offsetof(emulator_t, has_X) */
    void    (*tick)(struct emulator_s* emu, int cycles);             /* avance temporelle (optionnelle) */
} io_device_t;
```

La table `io_bus[]` (`src/io/io_bus.c`) est écrite en **initialiseurs désignés**
indexés par un `enum` (`DEV_LOCI`, `DEV_ACIA`…) : l'ordre de dispatch se lit dans
l'`enum`, et l'ordre des ticks (`io_bus_tick_order[]`) référence les entrées par
ce même nom.

**Pourquoi `emulator_t*` et pas un simple `self` ?** Parce que les `claims`
sont conditionnels/croisés : `microdisc.claims` doit savoir si l'ACIA est
présente ; l'ACIA à $0380 doit consulter la fiabilité MIA du LOCI. Le contexte
complet est nécessaire. (Un `self` seul suffirait pour un device isolé, mais
pas pour le graphe de priorités réel.)

**`write` renvoie « consommé »**, `claims_write` distinct.** L'écriture peut
**décliner** (renvoyer `false`) pour retomber sur le VIA — indispensable à
l'ULA-NG, qui doit *voir* les écritures de sa fenêtre même verrouillée (pour
guetter la séquence de déverrouillage 'N','G') tout en laissant passer les
octets neutres à l'identique du VIA. Comme son claim d'écriture (fenêtre seule)
diffère de son claim de lecture (déverrouillée + fenêtre), un `claims_write`
optionnel s'ajoute (NULL → réutilise `claims`). Les périphériques à plage
exclusive laissent `claims_write` à NULL et renvoient toujours `true`.

**Dispatch** (`main.c`) : une table `io_bus[]`. En lecture, `io_bus_find(emu, addr)`
renvoie le **premier** device qui `claims()` ; en écriture, `io_bus_find_write`
utilise `claims_write ?: claims`, puis le dispatch respecte le verdict de `write`
(false → repli VIA). L'ordre de la table = la priorité. `io_read/write_callback`
se réduisent à **la boucle du bus + le repli VIA** — tous les périphériques à
plage sont désormais dans la table (**pattern strangler** mené à son terme).

## 4. Étape 1 réalisée — preuve du modèle

**Digitelec DTL 2000** ($03F8-$03FD, **plage exclusive** → migration
iso-comportement, aucun risque de priorité) migré derrière `io_device_t` :
wrappers `dtl2000_dev_{claims,read,write}`, entrée dans `io_bus[]`, `if` en dur
retirés des 2 callbacks. **Suite complète verte** (test-dtl2000 15/15 + intégration).

## 5. Ordre de migration

1. ✅ **DTL 2000** (fait — plage exclusive, valide le contrat).
2. ✅ **ACIA 6551**, **Mageco / ORICON** : petites plages recouvrant le Microdisc
   → priorité encodée dans l'ordre de la table + les `claims` (ACIA possède
   $031C-$031F si présente ; l'ACIA à $0380 consulte la fiabilité MIA du LOCI).
3. ✅ **Microdisc** : `claims` = `has_microdisc && 0x0310-0x031F` (l'ACIA, placée
   avant, possède déjà $031C-$031F si présente). Le wrapper conserve `fdc_trace`
   et la synchro des drapeaux d'overlay (`basic_rom_disabled`/`overlay_active`).
4. ✅ **LOCI** (MIA $03A0-$03BF + TAP $0315-$0317 + DSK $0310-$0314/$0318-$0319,
   3 sous-plages **disjointes**) : un seul `io_device_t` **en tête de table** qui
   dispatche en interne. Le `claims` encode la priorité (TAP recouvre toujours le
   Microdisc ; DSK seulement `!has_microdisc`). Le snoop VIA ORB $0300
   (`loci_tap_motor`, ligne moteur cassette) n'est **pas** un claim → reste dans
   le chemin VIA.
5. ✅ **ULA-NG** : migrée grâce à l'extension du contrat (`write` renvoyant
   « consommé » + `claims_write` distinct). Lecture : `claims` = déverrouillée &&
   en fenêtre. Écriture : `claims_write` = en fenêtre (toujours) ; `ula_ng_dev_write`
   renvoie le verdict de `ula_ng_write` (false verrouillée → repli VIA) et
   synchronise l'IRQ raster quand l'écriture est consommée. Placée **en dernier**
   dans la table (repli avant VIA). Non-régression : boots déverrouillage+palette
   byte-identiques au pré-migration, `test-ula-ng` 60/60, garde visible 2/2.

Aujourd'hui, `io_read/write_callback` = **la boucle du bus + le repli VIA**. Tous
les périphériques à plage (LOCI et ULA-NG compris) sont sur `io_device_t`.

## 6. Étapes suivantes (au-delà du dispatch I/O)

Le même principe s'étend à ce qui rend main.c monolithique :

- **savestate** : ✅ *hook amorcé*. Le contrat porte `save_tag` + `save(emu,fp)`
  + `load(emu,fp,size)`. `savestate.c` reçoit la table via
  `savestate_set_io_devices()` (couplage évité), écrit une section par device qui
  fournit un hook, et au chargement route les tags inconnus vers le `load` du
  device correspondant. `save` peut renvoyer **false → aucune section** (état par
  défaut → `.ost` byte-identique, zéro régression). **Premier device migré :
  l'ULA-NG** (section « UNG ») — comble une vraie lacune (son état n'était pas
  persisté). Sérialisation en **blob** (POD sans pointeur) avec **garde par
  taille** au chargement ⇒ savestate *même-build* (le cas quicksave/load ; un
  `.ost` d'un autre build/arch est ignoré, jamais corrompu). Restent à migrer sur
  ce modèle : DTL2000, Mageco (petits jeux de registres) ; **LOCI a une réserve
  réelle** — ses handles de fichiers OS ne sont pas sérialisables tels quels.
  À terme, on pourra retirer les sections codées en dur (le mal OCB/OGP).
  **DTL2000 et Mageco migrés (Epic 7/US4)** : sections « DTL »/« MAG », état
  émulé en blob + **pointeurs hôte préservés** au chargement (backend/trace/
  callbacks non sérialisables) ; transport live non restauré (même-build). Reste
  **LOCI** : réserve réelle (montages/descripteurs OS).
- **tick** (Epic 7/US5, puis sprint D 2.4.0) : ✅ *dans le contrat*. Chaque
  device fournit `tick` et `present_off` ; `io_bus_tick()` parcourt
  `io_bus_tick_order[]`, un ordre **explicite, distinct de l'ordre de dispatch**
  et identique à l'ordre historique (microdisc → jasmin → loci → acia → dtl →
  mageco → sp0256 → mea8000) → iso-comportement par construction
  (`tools/cli_golden.sh` : 0 écart). Coût mesuré, car la fonction tourne à
  **chaque cycle** : boucle naïve +22 % par trame, déroulée +7 %, déroulée avec
  `__builtin_expect(présent, 0)` → dans le bruit (le compilateur la replie en
  tests de drapeaux et appels directs, disposition identique à l'ancien code).
- **savestate, sprint D (2.4.0)** : sections **« JAS »** (Jasmin : état FDC,
  verrous, cartes de secteurs défectueux ; les images passent par « DSK »,
  désormais écrite aussi pour le Jasmin), **« SPO »** (SP0256 : séquenceur,
  filtre LPC-12, échantillons en attente) et **« MEA »** (MEA8000 : séquenceur,
  4 formants, anneau audio). Sérialisation **champ par champ, petit-boutiste,
  versionnée** (`include/utils/binio.h`) — pas de blob : ni ROM, ni tables
  recalculables (~92 Ko pour le MEA8000), ni pointeurs hôte dans le fichier ;
  section de version ou de taille inattendue ignorée. L'état du WD1793 est
  factorisé (`fdc_state_save/load`, format **v2** : + âge du DRQ et analyseur de
  formatage, un `.ost` v1 se relit) et `fdc_state_resume()` recalcule le
  pointeur du secteur en cours : une reprise **en plein transfert disque** lisait
  « Record Not Found » (défaut ancien, Microdisc compris), corrigé et couvert par
  `test-savestate-determinism` (Jasmin sauvé en plein boot TDOS).
- **Restent hors savestate** : **LOCI** (handles de fichiers OS, montages) et
  l'ACIA, dont la section « SER » reste écrite en dur par `savestate.c`
  (registres seulement, le transport hôte n'est pas restauré).
- **init / reset / cleanup** : hooks *non ajoutés au contrat*. Raison honnête :
  le reset n'est **pas uniforme** (le warm reset F5 ne reset que CPU + LOCI, ce
  dernier « garde les montages ») → une boucle de reset générique changerait le
  comportement. Décider quels périphériques voient la ligne /RESET relève de la
  fidélité matérielle (schémas à l'appui), pas d'un refactor.

## 6 bis. Définition de « terminé » pour un nouveau périphérique de bus

1. une entrée dans `io_bus[]` (et l'`enum` `DEV_*`) avec `claims`/`read`/`write`,
   `present_off` et, s'il avance dans le temps, `tick` placé à sa position dans
   `io_bus_tick_order[]` ;
2. une section `.ost` (`save_tag`/`save`/`load`) sérialisant **l'état émulé
   seulement**, versionnée, qui ignore une section inconnue ;
3. un test unitaire dédié, dont un test « chaque champ est restauré » (état
   source rempli de valeurs distinctives → rechargé → resauvegardé → mêmes octets) ;
4. un scénario dans `tests/integration/test_savestate_determinism.py` si le
   périphérique influence l'exécution du 6502 ;
5. un cas dans `tests/cli_golden/cases.txt` pour ses options.

## 7. Contrainte opérationnelle

L'exécutable `oric1-emu` est utilisé par d'autres programmes : **jamais de
`make clean`** pendant ce chantier (il efface le binaire) ; builds **incrémentaux**
uniquement (le binaire n'est remplacé qu'en cas de link réussi) ; chaque étape
laisse un `oric1-emu` fonctionnel et **iso-comportement**.

## 8. Références
- `include/io/io_device.h`, `src/main.c` (`io_bus[]`, `io_bus_find`).
- Symptôme d'origine : `docs/ocula/CODE-MAP.md` (retrait OCULA, même mal).
