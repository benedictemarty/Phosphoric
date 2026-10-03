# 0006 — Expansion cards as self-registering modules, explicit list

- **Status**: proposed (2026-10-03); plan: `docs/specs/CARD_MODULES.md`

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

- Adding a card: one file + one line (target, proven by a test in G7).
- Step-by-step migration (G1 MEA8000 pilot → G6 LOCI), each step proven identical
  (`cli_golden`, `.ost` byte for byte, performance bench).
- If the G1 pilot does not pay off, the decision is rejected and recorded here.
