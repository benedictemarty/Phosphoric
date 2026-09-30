# Architecture — I/O bus & peripherals (`io_device_t`)

> **Branch `feature/io-device-bus`.** Moves out of `main.c` the cascade of `if
> (has_X && X_addr_in_range(addr)) return X_read(...)` (the hard-wired page 3
> dispatch, ~25 peripherals) in favour of a **peripheral table**. Goal:
> adding/removing a peripheral = registering/removing **one entry**, without
> touching the core. Removing OCULA showed the cost of lacking this
> abstraction (code sprinkled across main/memory/video/savestate).

## 1. The problem

`main.c` (~4600 lines) is a god object: every peripheral is re-wired into it by
hand in 5+ places (CLI, init, **I/O dispatch**, main loop,
savestate, glue). `io_read_callback`/`io_write_callback` are two cascades of
`if`s with priorities and **cross-dependencies** (ACIA↔Microdisc, ORICON window
overlapping the Microdisc, LOCI MIA reliability for the ACIA at $0380…).

## 2. What is a "bus peripheral" (and what is not)

Criterion: **claims an address range in page 3** (± overlay ROM).

- **Core** (do NOT treat as a peripheral): 6502, memory, **VIA**, ULA,
  PSG, keyboard. Soldered in, they are the Oric.
- **Bus devices** (target of `io_device_t`): **Microdisc** ($0310-$031F),
  **ACIA 6551** ($031C-$031F), **ULA-NG** ($0340-$035F), **LOCI**
  ($03A0-$03BF + TAP/DSK), **DTL 2000** ($03F8-$03FD), **Mageco/ORICON**
  ($03FE-$03FF / $031C-$031E).
- **Port-attached** (peripherals, but NOT bus devices): joystick
  (PSG Port A), printer/MCP-40 (VIA Port A + CA2), cassette (VIA CB1). They
  are driven through the existing port callbacks — **outside** this abstraction.

## 3. The contract

`include/io/io_device.h`:

```c
typedef struct io_device_s {
    const char* name;
    bool    (*claims)(struct emulator_s* emu, uint16_t addr);        /* READ claim (+ write by default) */
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

**`write` returns "consumed"**, with a separate `claims_write`.** A write can
**decline** (return `false`) to fall back to the VIA — essential for the
ULA-NG, which must *see* writes to its window even while locked (to
watch for the 'N','G' unlock sequence) while letting neutral
bytes through exactly as the VIA would. Since its write claim (window only)
differs from its read claim (unlocked + window), an optional `claims_write`
is added (NULL → reuses `claims`). Peripherals with an exclusive range
leave `claims_write` NULL and always return `true`.

**Dispatch** (`main.c`): an `io_bus[]` table. On read, `io_bus_find(emu, addr)`
returns the **first** device whose `claims()` matches; on write, `io_bus_find_write`
uses `claims_write ?: claims`, then the dispatch honours the verdict of `write`
(false → fall back to VIA). Table order = priority. `io_read/write_callback`
shrink to **the bus loop + the VIA fallback** — all range-based peripherals are
now in the table (**strangler pattern** carried through to the end).

## 4. Step 1 done — proof of the model

**Digitelec DTL 2000** ($03F8-$03FD, **exclusive range** → behaviour-identical
migration, no priority risk) migrated behind `io_device_t`:
wrappers `dtl2000_dev_{claims,read,write}`, entry in `io_bus[]`, hard-wired `if`s
removed from the 2 callbacks. **Full suite green** (test-dtl2000 15/15 + integration).

## 5. Migration order

1. ✅ **DTL 2000** (done — exclusive range, validates the contract).
2. ✅ **ACIA 6551**, **Mageco / ORICON**: small ranges overlapping the Microdisc
   → priority encoded in the table order + the `claims` (the ACIA owns
   $031C-$031F if present; the ACIA at $0380 consults the LOCI MIA reliability).
3. ✅ **Microdisc**: `claims` = `has_microdisc && 0x0310-0x031F` (the ACIA, placed
   earlier, already owns $031C-$031F if present). The wrapper keeps `fdc_trace`
   and the synchronisation of the overlay flags (`basic_rom_disabled`/`overlay_active`).
4. ✅ **LOCI** (MIA $03A0-$03BF + TAP $0315-$0317 + DSK $0310-$0314/$0318-$0319,
   3 **disjoint** sub-ranges): a single `io_device_t` **at the head of the table** that
   dispatches internally. Its `claims` encodes the priority (TAP always overlaps the
   Microdisc; DSK only when `!has_microdisc`). The VIA ORB $0300 snoop
   (`loci_tap_motor`, cassette motor line) is **not** a claim → it stays in
   the VIA path.
5. ✅ **ULA-NG**: migrated thanks to the contract extension (`write` returning
   "consumed" + separate `claims_write`). Read: `claims` = unlocked &&
   in window. Write: `claims_write` = in window (always); `ula_ng_dev_write`
   returns the verdict of `ula_ng_write` (false when locked → fall back to VIA) and
   synchronises the raster IRQ when the write is consumed. Placed **last**
   in the table (fallback before the VIA). Non-regression: unlock+palette boots
   byte-identical to pre-migration, `test-ula-ng` 60/60, visible guard 2/2.

Today, `io_read/write_callback` = **the bus loop + the VIA fallback**. All
range-based peripherals (LOCI and ULA-NG included) are on `io_device_t`.

## 6. Next steps (beyond I/O dispatch)

The same principle extends to what makes main.c monolithic:

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

## 7. Operational constraint

The `oric1-emu` executable is used by other programs: **never
`make clean`** during this work (it deletes the binary); **incremental** builds
only (the binary is only replaced on a successful link); each step
leaves a working, **behaviour-identical** `oric1-emu`.

## 8. References
- `include/io/io_device.h`, `src/main.c` (`io_bus[]`, `io_bus_find`).
- Original symptom: `docs/ocula/CODE-MAP.md` (OCULA removal, same problem).
