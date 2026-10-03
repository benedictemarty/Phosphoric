# Expansion cards as self-registering modules (sprint G)

- **Statut** : en cours — G1 livré en 2.13.0 (pilote MEA8000). Décision : ADR 0006
  (acceptée).
- **But** : ajouter une carte d'extension = **un fichier** (la carte) **+ une ligne**
  (la liste des cartes), au lieu des ~10 fichiers d'aujourd'hui.
- **Hors périmètre** : le chargement dynamique (`.so`/`.dll`, `dlopen`). Il exigerait
  une ABI stable, compliquerait les builds WASM et Windows, et poserait la question
  de la confiance envers le code chargé ; sans demande de tiers, pas de besoin.

## 1. Current state (measured on 2.12.9)

The I/O contract already exists (`io_device_t`, `include/io/io_device.h`):
`claims`, `read`, `write`, `peek`, `tick`, `save`/`load` of a `.ost` section. The
bus (`src/io/io_bus.c`) and the F1 menu (`src/cards.c`) are generic. But assembly
is static and scattered. Example, the MEA8000 card (TMPI speech synthesis), a
"simple" card:

| Role | Where |
|---|---|
| Options `--mea8000`, `--mea8000-addr` | `cli_options.h` (getopt table + `OPT_*`), `cli_opts.h` (fields), `cli_opts.c` (defaults), `cli_args.c` (parsing), `cli_usage.c` (help) |
| State and presence | `emulator.h` (`mea8000_t mea8000; bool has_mea8000;`) |
| Bus | `io_bus.c` (`*_dev_*` wrappers, `io_bus[]` entry, rank in `io_bus_tick_order[]`) |
| Setup, address conflicts | `main.c` (`main_setup_disks_speech`) |
| Audio mixing | `audio_output.c` (real time), `main.c` (WAV capture) |
| F1 menu | `cards.c` (`k_cards[]`) |

Footprint per card (references to `has_X` / `emu->X`):

| Card | Refs | Files | Family |
|---|---|---|---|
| Mageco, SP0256, MEA8000, DTL 2000, ULA-NG | 17-25 | 3 (+ options) | simple |
| ACIA 6551 | 52 | 5 | serial (transports, debugger) |
| Jasmin, Microdisc | 47 / 80 | 7 / 8 | disk (media, menu, debugger) |
| LOCI | 111 | 10 | disk + coprocessor |

## 2. Target

One descriptor per card, in the card's file (`src/cards/card_<id>.c`):

```c
typedef struct card_module_s {
    const card_desc_t*  desc;        /* F1 menu: name, role, group, explained parameters */
    const card_opt_t*   opts;        /* launch options: name, argument, help, field */
    size_t              cfg_size;    /* own configuration (filled by CLI parsing) */
    void (*cfg_defaults)(void* cfg);
    int  (*setup)(emulator_t* emu, const void* cfg);  /* presence, conflicts, init */
    void (*teardown)(emulator_t* emu);
    const io_device_t*  bus;         /* existing I/O contract, unchanged */
    int                 dispatch_rank, tick_rank;     /* EXPLICIT orders (ADR 0003) */
    void (*audio)(emulator_t* emu, int16_t* buf, int n); /* NULL: no sound */
} card_module_t;
```

and a list, the only other place to change:

```c
/* src/cards/cards_all.c */
const card_module_t* const k_card_modules[] = { &card_mea8000, &card_sp0256, ... };
```

The core derives from it: getopt table (card options appended to the core ones),
help, `phosphoric.cfg`, F1 menu, `io_bus[]`, tick order, audio mixing.

**Explicit list rather than automatic registration** (`__attribute__((constructor))`
constructors, linker sections): these mechanisms are fragile across GCC/MinGW,
clang/macOS and emscripten (unspecified order, objects dropped from a static
library). One line per card, readable and deterministic.

### Réalisation (G1, 2.13.0)

Le pilote a fait évoluer la cible sur trois points :

- **La liste est une macro**, `include/cards_list.h` : `X(mea8000, 1)` (id, tick).
  Une table de pointeurs lue à l'exécution coûtait **+5,3 % d'instructions**
  (mesuré : `tools/instr_count.sh`) : l'appel du tick d'une carte, à chaque cycle,
  devenait indirect. Générées depuis la macro, les ticks des cartes sont des
  appels directs avec un test de présence à position fixe (`include/card_ticks.h`,
  `emu->card_on[CARD_IDX_<id>]`) : **même nombre d'instructions** que la référence
  (874 033 056 contre 874 025 099 sans carte, 905 086 069 contre 905 078 314 avec
  MEA8000, sur 2 M cycles).
- **Ancres** : chaque carte dit avant quelle option (`opts_before`), quel bloc
  d'aide (`help_before`), quelle fiche du menu (`desc_before`) et quel
  périphérique (`bus_before`) elle se place ; l'aide, les messages d'options
  ambiguës (« possibilities: '--mea8000' '--mea8000-addr' »), le menu et l'ordre
  des sections `.ost` restent identiques. Sans ancre : en fin de liste.
- **Le Makefile prend `src/cards/card_*.c` par motif** : une carte = son fichier +
  sa ligne dans `cards_list.h`.
- Son : `audio_add_source()` (`src/audio/audio_sources.c`), mixé par le callback SDL
  et par la capture avec le même découpage qu'avant (sortie identique).
- Mise en route : `card_modules_setup(emu, cfgs, étape)` à la place exacte du
  code d'origine (`CARD_STAGE_SPEECH` pour MEA8000).

Preuves G1 : `cli_golden` 125 cas (dont 9 nouveaux : préfixes ambigus, conflits,
`.ost` et WAV avec la carte) sans écart avec 2.12.9 ; instructions identiques ;
builds SDL2=0, HTTPAPI=1, WASM ; `make tests-strict` et `SANITIZE=1`.

## 3. Invariants (vérifiés à chaque étape)

1. **Comportement identique** : `cli_golden` (116 lignes de commande, sorties et
   fichiers produits à l'octet) entre le binaire d'avant et celui d'après ; aide
   (`--help`) identique au caractère près.
2. **Sauvegardes d'état** : mêmes tags de section `.ost`, fichiers identiques à
   l'octet ; anciens `.ost` relus (`test-loadstate`, `test-savestate-determinism`).
3. **Performance** : pas de boucle générique dans le chemin par cycle (+22 %
   mesuré, ADR 0003). Les tables restent `const` et l'ordre des ticks explicite ;
   nombre d'instructions comparé à la référence (`tools/instr_count.sh`, stable,
   contrairement au temps sur une machine bridée) et `make test-bench`.
4. **Configuration** : clés de `phosphoric.cfg` (`carte.*`, `<id>.<param>`)
   inchangées ; menu F1 identique (`test-iomenu`, `test-cards`, `test-web-iomenu`).
5. Builds Linux, macOS, Windows (MinGW) et WASM ; `make SANITIZE=1 tests-strict`.

## 4. Steps

| Step | Content | Proof |
|---|---|---|
| **G1** ✅ 2.13.0 Contrat + pilote | `card_module_t`, `cards_all.c` ; analyse CLI qui ajoute les options des modules ; **MEA8000** migrée de bout en bout (son état reste dans `emulator_t` à ce stade) | invariants 1-5 ; son : capture `--mea8000 --audio-wav` identique à l'octet avant/après |
| **G2** Cartes simples | SP0256, Mageco, DTL 2000, ULA-NG | invariants 1-5 par carte |
| **G3** État privé | les cartes simples gardent leur état derrière le module (`emu->card_state[i]`) ; `has_X` retirés de `emulator_t` pour elles | `emulator.h` ne connaît plus ces 5 cartes |
| **G4** ACIA | transports série (`--serial`, `--acia-addr`, IRQ…), débogueur (`peek`) | `test-serial-*`, `test-loci-acia-*`, `cli_golden` |
| **G5** Disques | Microdisc, Jasmin : médias via l'API `emu_disk_*` déjà en place | `test-storage`, `test-jasmin`, `test-control-media-swap` |
| **G6** LOCI | carte la plus couplée (coprocesseur, menu, fichiers hôte) ; périmètre réévalué après G5 | suites LOCI complètes |
| **G7** Garde-fou + guide | carte d'exemple (`card_demo.c`, hors build par défaut) ; test qui l'ajoute et vérifie qu'un fichier + une ligne suffisent ; guide « ajouter une carte » | `make test-card-template` |

Order: G1 alone first. If the MEA8000 pilot costs more in performance or
readability than it brings, we stop there (decision recorded in ADR 0006).

## 5. Risks

- **Option parsing**: the getopt table becomes dynamic; `OPT_*` codes assigned to
  modules at startup. Errors (unknown option, missing argument) must stay
  identical — covered by `cli_golden`.
- **Help order**: today's help orders the cards by hand; generated sections must
  reproduce the exact text, or the help change is accepted and recorded (the only
  exception allowed to invariant 1, to be decided in G1).
- **Cross couplings**: address conflicts between cards (MEA8000 / Mageco /
  SP0256), group exclusivity (disk) — today in `main.c` and `cards.c`; to be
  carried by the registry (`cards_conflict` already exists).
- **LOCI**: coupled to the keyboard, the tape and the media; it may only be
  partly modularisable.
