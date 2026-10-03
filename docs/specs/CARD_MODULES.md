# Cartes d'extension en modules auto-enregistrés (sprint G)

- **Statut** : plan proposé (2026-10-03), non commencé. Décision : ADR 0006.
- **But** : ajouter une carte d'extension = **un fichier** (la carte) **+ une ligne**
  (la liste des cartes), au lieu des ~10 fichiers d'aujourd'hui.
- **Hors périmètre** : le chargement dynamique (`.so`/`.dll`, `dlopen`). Il exigerait
  une ABI stable, compliquerait les builds WASM et Windows, et poserait la question
  de la confiance envers le code chargé ; sans demande de tiers, pas de besoin.

## 1. État des lieux (mesuré sur 2.12.9)

Le contrat d'E/S existe déjà (`io_device_t`, `include/io/io_device.h`) : `claims`,
`read`, `write`, `peek`, `tick`, `save`/`load` d'une section `.ost`. Le bus
(`src/io/io_bus.c`) et le menu F1 (`src/cards.c`) sont génériques. Mais
l'assemblage est statique et dispersé. Exemple, la carte MEA8000 (synthèse vocale
TMPI), carte « simple » :

| Rôle | Où |
|---|---|
| Options `--mea8000`, `--mea8000-addr` | `cli_options.h` (table getopt + `OPT_*`), `cli_opts.h` (champs), `cli_opts.c` (défauts), `cli_args.c` (analyse), `cli_usage.c` (aide) |
| État et présence | `emulator.h` (`mea8000_t mea8000; bool has_mea8000;`) |
| Bus | `io_bus.c` (enveloppes `*_dev_*`, entrée de `io_bus[]`, rang dans `io_bus_tick_order[]`) |
| Mise en route, conflits d'adresses | `main.c` (`main_setup_disks_speech`) |
| Mixage audio | `audio_output.c` (temps réel), `main.c` (capture WAV) |
| Menu F1 | `cards.c` (`k_cards[]`) |

Empreinte par carte (références à `has_X` / `emu->X`) :

| Carte | Réf. | Fichiers | Famille |
|---|---|---|---|
| Mageco, SP0256, MEA8000, DTL 2000, ULA-NG | 17-25 | 3 (+ options) | simple |
| ACIA 6551 | 52 | 5 | série (transports, débogueur) |
| Jasmin, Microdisc | 47 / 80 | 7 / 8 | disque (médias, menu, débogueur) |
| LOCI | 111 | 10 | disque + coprocesseur |

## 2. Cible

Un descripteur par carte, dans le fichier de la carte (`src/cards/card_<id>.c`) :

```c
typedef struct card_module_s {
    const card_desc_t*  desc;        /* menu F1 : nom, rôle, groupe, paramètres expliqués */
    const card_opt_t*   opts;        /* options de lancement : nom, argument, aide, champ */
    size_t              cfg_size;    /* configuration propre (remplie par l'analyse CLI) */
    void (*cfg_defaults)(void* cfg);
    int  (*setup)(emulator_t* emu, const void* cfg);  /* présence, conflits, init */
    void (*teardown)(emulator_t* emu);
    const io_device_t*  bus;         /* contrat d'E/S existant, inchangé */
    int                 dispatch_rank, tick_rank;     /* ordres EXPLICITES (ADR 0003) */
    void (*audio)(emulator_t* emu, int16_t* buf, int n); /* NULL : pas de son */
} card_module_t;
```

et une liste, seul autre endroit à modifier :

```c
/* src/cards/cards_all.c */
const card_module_t* const k_card_modules[] = { &card_mea8000, &card_sp0256, ... };
```

Le cœur en dérive : table getopt (options des cartes ajoutées à celles du cœur),
aide, `phosphoric.cfg`, menu F1, `io_bus[]`, ordre des ticks, mixage audio.

**Liste explicite plutôt qu'enregistrement automatique** (constructeurs
`__attribute__((constructor))`, sections du linker) : ces mécanismes sont fragiles
entre GCC/MinGW, clang/macOS et emscripten (ordre non spécifié, objets éliminés
d'une bibliothèque statique). Une ligne par carte, lisible et déterministe.

## 3. Invariants (vérifiés à chaque étape)

1. **Comportement identique** : `cli_golden` (116 lignes de commande, sorties et
   fichiers produits à l'octet) entre le binaire d'avant et celui d'après ; aide
   (`--help`) identique au caractère près.
2. **Sauvegardes d'état** : mêmes tags de section `.ost`, fichiers identiques à
   l'octet ; anciens `.ost` relus (`test-loadstate`, `test-savestate-determinism`).
3. **Performance** : pas de boucle générique dans le chemin par cycle (+22 %
   mesuré, ADR 0003). Les tables restent `const` et l'ordre des ticks explicite ;
   `make test-bench` (budget par trame) à chaque migration.
4. **Configuration** : clés de `phosphoric.cfg` (`carte.*`, `<id>.<param>`)
   inchangées ; menu F1 identique (`test-iomenu`, `test-cards`, `test-web-iomenu`).
5. Builds Linux, macOS, Windows (MinGW) et WASM ; `make SANITIZE=1 tests-strict`.

## 4. Étapes

| Étape | Contenu | Preuve |
|---|---|---|
| **G1** Contrat + pilote | `card_module_t`, `cards_all.c` ; analyse CLI qui ajoute les options des modules ; **MEA8000** migrée de bout en bout (son état reste dans `emulator_t` à ce stade) | invariants 1-5 ; son : capture `--mea8000 --audio-wav` identique à l'octet avant/après |
| **G2** Cartes simples | SP0256, Mageco, DTL 2000, ULA-NG | invariants 1-5 par carte |
| **G3** État privé | les cartes simples gardent leur état derrière le module (`emu->card_state[i]`) ; `has_X` retirés de `emulator_t` pour elles | `emulator.h` ne connaît plus ces 5 cartes |
| **G4** ACIA | transports série (`--serial`, `--acia-addr`, IRQ…), débogueur (`peek`) | `test-serial-*`, `test-loci-acia-*`, `cli_golden` |
| **G5** Disques | Microdisc, Jasmin : médias via l'API `emu_disk_*` déjà en place | `test-storage`, `test-jasmin`, `test-control-media-swap` |
| **G6** LOCI | carte la plus couplée (coprocesseur, menu, fichiers hôte) ; périmètre réévalué après G5 | suites LOCI complètes |
| **G7** Garde-fou + guide | carte d'exemple (`card_demo.c`, hors build par défaut) ; test qui l'ajoute et vérifie qu'un fichier + une ligne suffisent ; guide « ajouter une carte » | `make test-card-template` |

Ordre : G1 seul d'abord. Si le pilote MEA8000 coûte en performance ou en lisibilité
plus qu'il ne rapporte, on s'arrête là (décision notée dans l'ADR 0006).

## 5. Risques

- **Analyse des options** : la table getopt devient dynamique ; codes `OPT_*`
  attribués aux modules au démarrage. Les erreurs (option inconnue, argument
  manquant) doivent rester identiques — couvert par `cli_golden`.
- **Ordre de l'aide** : l'aide actuelle range les cartes à la main ; les sections
  générées doivent reproduire le texte exact, ou le changement d'aide est assumé
  et noté (seule exception tolérée à l'invariant 1, à décider en G1).
- **Couplages croisés** : conflits d'adresses entre cartes (MEA8000 / Mageco /
  SP0256), exclusivités de groupe (disque) — aujourd'hui dans `main.c` et
  `cards.c` ; à porter par le registre (`cards_conflict` existe déjà).
- **LOCI** : couplée au clavier, à la cassette et aux médias ; il est possible
  qu'elle ne puisse être qu'en partie modularisée.
