# 0006 — Expansion cards as self-registering modules, explicit list

- **Status**: accepted (2.13.0, G1 MEA8000 pilot); plan: `docs/specs/CARD_MODULES.md`

## Context

Expansion peripherals already share an I/O contract (`io_device_t`) and a
registry for the F1 menu (`cards.c`), but they are still assembled by hand: a
simple card such as MEA8000 touches about ten files (launch options, state in
`emulator_t`, bus, setup, audio, menu). Adding a card is slow and error-prone.

## Proposed decision

Each card is described by a `card_module_t` descriptor in its own file (menu,
options, configuration, setup, bus contract, dispatch and tick ranks, audio). An
explicit list `k_card_modules[]` enumerates them; the core derives options, help,
configuration, menu and bus tables from it.

- No dynamic loading (`dlopen`): stable ABI, WASM, Windows, trust.
- No registration through constructors or linker sections: not portable across
  GCC/MinGW, clang/macOS and emscripten; one line per card is enough.
- Dispatch and tick orders stay explicit (ADR 0003), and the per-cycle path stays
  free of any generic loop.

## Consequences

- Ajouter une carte : un fichier + une ligne — prouvé par `make test-card-template`
  (2.19.0) ; guide `docs/CARTES.md`.
- Réalisation G1 : la liste est une macro (`include/cards_list.h`) et non une table
  de pointeurs : celle-ci rendait indirect le tick par cycle (+5,3 % d'instructions
  mesurées) ; générés depuis la macro, les ticks restent des appels directs
  (instructions identiques à la référence).
- Migration par étapes (G1 pilote MEA8000 → G6 LOCI), chacune prouvée identique
  (`cli_golden`, `.ost` à l'octet, banc de performance).
- Si le pilote G1 n'apporte pas assez, la décision est rejetée et notée ici.
