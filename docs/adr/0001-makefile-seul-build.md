# 0001 — The Makefile is the only build system

- **Status**: accepted (2.1.3, sprint A of the architecture plan)

## Context

The repository had both a `Makefile` and a `CMakeLists.txt`. The latter was no
longer maintained: twenty sources were missing from it and linking failed. Two
build systems always drift apart; only one was actually used (CI, tests, Windows
distribution, WebAssembly).

## Decision

`CMakeLists.txt` is removed; the `Makefile` is the only build. Since 2.2.0
(sprint B), objects are built out of the source tree, one directory per
configuration (`build/<config>/`: SDL2, HTTPAPI, CAST, MIDI, TLS, TUI, LOCI
backend, DEBUG, COVERAGE, SANITIZE, VIA_NO_LAZY), and tests link objects
(`UNIT_TEST` / `DIRECT_TEST` macros).

## Consequences

- No IDE project generation (CMake produced them).
- Changing options never mixes objects from different configurations (the
  "`make SDL2=0` then `make tests`" trap is gone by construction).
- Every new source must be added to the `Makefile` (`SOURCES` list and, if a test
  compiles it directly, that test's list).
