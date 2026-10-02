# Architecture audit — Phosphoric

> Critical audit of 2026-07-17 (v1.84.0-alpha). Based on real measurements
> (include graph, breakdown of `main.c`, tick order, threads).
> `file:line` references verified or marked "approx.". This document is
> deliberately **critical**: it lists what works, but above all the debt.

> **Bilan 2026-10-02 (2.12.0)** — plan d'amélioration (sprints A à F) mené :
> - **§ 2.1** `main()` : 2 043 → 32 lignes (2.3.0, sprint C) ; options dans
>   `cli_opts_t` / `cli_parse_args()`, 14 étapes `main_setup_*` ; `main.c` reste le
>   fichier d'assemblage (≈ 4 460 l), mais découpé en étapes nommées.
> - **§ 2.3** contrat `io_device_t` : `tick` + `present_off`, ordre explicite
>   `io_bus_tick_order[]` (2.4.0, sprint D, ADR 0003) ; sections `.ost` par
>   périphérique pour ULA-NG, Mageco, DTL 2000, Jasmin, SP0256, MEA8000 (LOCI et
>   transports série : non sérialisables, § 2.2 inchangé).
> - **§ 2.4** monolithes secondaires : `control.c` 1 352 → 261 l + `control_util.c`
>   et `control_cmd_{mem,debug,media}.c` ; `debugger.c` 2 395 → 637 l +
>   `debugger_{view,mem,asm,repl}.c` (2.12.0, sprint F). Déplacement mécanique,
>   prouvé par `cli_golden` (116 lignes de commande, 0 écart) et la suite complète.
>   `process_repl_line()` (≈ 850 l, une seule fonction) découpé en 2.12.3 : 40
>   gestionnaires `repl_*()` et une table de 53 noms ; sortie identique sur un script
>   REPL couvrant toutes les commandes.
> - **§ 4** sécurité des serveurs réseau : auditée (2.8.0-2.9.0) — API HTTP sur
>   127.0.0.1 par défaut, stub GDB passé sur 127.0.0.1, analyse HTTP fuzzée ; cast et
>   modem en écoute exposés par nécessité. Sanitizers ASan/UBSan et fuzzing de tous
>   les lecteurs de fichiers en CI.
> - Décisions consignées dans `docs/adr/` (0001-0005).

## 0. Verdict in one sentence

The emulator's **core** is cleanly modularised (decoupled layers, zero include
cycles, correct per-cycle IRQ/timing), but the **periphery** is
concentrated in a 4690-line `main.c` (of which `main()` ≈ 1487) and the
`io_device_t` contract is **incomplete** (dispatch migrated, but lifecycle and
savestate only partial). The debt is *peripheral*, not *central* — which is
good news: it can be tackled by extraction, without touching the engine.

## 1. What is solid (measured)

### 1.1 Decoupling of the base modules
`cpu`, `memory`, `video`, `audio`, `storage` **do not include** `emulator.h`
(0 dependency): each one operates on its own struct passed by pointer. Out of ~166
files, only **18** include `emulator.h` (~11 %). This is what makes the
"one executable per module" test framework possible.

### 1.2 Zero include cycles
The graph is a DAG. Potential cycles are broken by deliberate
**forward declarations**: `cpu ↔ memory` (`cpu6502.h`), `video ↔ ula_ng` (`video.h:12`),
`cassette ↔ via6522` (`cassette.h`). `io_device.h` forward-declares `emulator_s`.

### 1.3 Per-cycle timing fidelity
Each 6502 memory access triggers `cpu_cycle_tick()` (src/main.c ≈ 1291),
which advances VIA, FDC, ACIA, DTL2000, Mageco **per cycle** — not in a batch
after the instruction. Multi-source **level-triggered** IRQ via the `IRQF_*` bitfield
(cpu6502.h): correct, each source manages its own bit.

### 1.4 Defensive concurrency
3 auxiliary threads (MJPEG cast, HTTP API, CASTV2 heartbeat). **None** mutates
`emulator_t` directly: everything goes through `control_queue` (MPSC, mutex +
condition variable per command), drained at the *frame boundary* by the main loop
(single consumer). The framebuffer is **copied under mutex** once per frame
to the cast (`cast_server_push_frame`), never shared raw. Serial backends use
**non-blocking** I/O (no thread). No race identified.

### 1.5 Consistent conventions
Include guards, `snake_case`/`_t`/`_s`, Doxygen headers, K&R 4 spaces:
consistent across the whole repository.

## 2. The debt (prioritised)

> **Update 2026-09-12 (2.0.3)**: `emulator_run()` (1,296 lines) has been split
> into 17 named steps around a `run_state_t` (126 lines), without changing the
> execution order — the caveat of § 3 ("do not modernise it into an event loop")
> remains valid and respected: it is an extraction, not a redesign. `main.c` is
> 4,536 lines; what remains is `main()` and its option parser (US3 of Epic 7, not done).

### 2.1 🔴 `main.c` = peripheral god object (4690 L, `main()` ≈ 1487 L)
`main()` does everything: ~79 CLI options (getopt_long), subsystem initialisation,
creation of the serial backends (≈ 390 L for modem/digitelec/picowifi), main
loop, SDL events, function keys, teardown. 79 functions in a
single file, many of them **wiring callbacks** (io_read/write_callback,
keyboard_matrix_read, the 12 `*_dev_*` wrappers, the `*_cpu_irq_set/clr` pairs).

**Honest nuance:** it is not a *behavioural* god object (the logic lives
in the modules) but an *assembly* god object. The risk is
readability/maintainability, not correctness.

### 2.2 🔴 Emulated state and host resources mixed in the same struct
`dtl2000_t`, `mageco_t` (and `serial_backend_t`) mix, in **a single
struct**, serialisable POD registers **and** non-serialisable handles
(`serial_backend_t* backend`, `FILE* trace`, `irq_*` callbacks, `sockfd`,
`master_fd`, ALSA handles). `pia6821_t` carries 7 callback pointers,
`acia6850_t` carries 2.

**Concrete (experienced) consequence:** this is what made the
DTL2000/Mageco/LOCI savestate **impossible to do cleanly** — a blob would corrupt the
pointers, and the transport (TCP/PTY connection, bytes in flight) cannot be
restored anyway. It is the structural debt most "worth" addressing.

### 2.3 🟠 Incomplete `io_device_t` contract (no lifecycle)
The contract covers `claims/read/write/save/load` but **not** `init/reset/tick`.
Therefore:
- I/O dispatch has indeed moved onto the bus, but **init/reset/tick remain
  hard-wired** in `main.c` (`cpu_cycle_tick`, reset paths);
- `save/load` is only implemented for **the ULA-NG**; the other 5 entries of
  `io_bus[]` have `save_tag=NULL`.

**Inconsistency to acknowledge:** the tick order (`VIA→cassette→microdisc→loci→acia→
dtl→mageco`) **differs** from the order of the `io_bus[]` table
(`loci→acia→mageco→microdisc→dtl→ula-ng`). Unifying tick + dispatch in a single
loop would require first proving byte-identical equivalence (the VIA must
be ticked first — it drives the timers).

### 2.4 🟠 Secondary monoliths — addressed in 2.12.0 (see the review at the top)
`debugger.c` (2382 L) and `control.c` (1290 L) are monolithic blocks
(debug REPL; parser + 30+ command handlers + dispatch).

### 2.5 🟡 Unused `emulator.h` includes (verified)
`src/io/microdisc.c:17` and `src/io/loci_core.c:16` include `emulator.h` with
**0 use** of `emulator_t`/`emu->`/`EMU_VERSION` → needless coupling to remove
(subject to recompilation).

### 2.6 🟡 ULA-NG raster resolution per scanline
The raster IRQ and the copper are evaluated per **logical scanline** (every 64
cycles), not mid-cycle. Acceptable for PAL 50 Hz, but coarser than the hardware
ULA — to be documented as a known limitation.

## 3. What NOT to refactor (risk > benefit)
- Decoupling of the base modules: already good.
- The root struct `emulator_t` (aggregation by value): a legitimate pattern.
- Duplication of the test macros: a deliberate "zero dependency" choice.
- `emulator_run()` (main loop, timing-critical): do not "modernise" it
  into an event loop without need — the gain is theoretical, the risk of timing
  regression is real.

## 4. Areas not audited (honesty)
Not covered by this audit, hence **not assessed**: *intra-instruction* cycle
precision (phase of the accesses), robustness of the cassette signal-mode gating on
ROM PC, detailed design of the control protocol beyond its form, security of the
exposed network servers.

## 5. Action plan → see Epic 7 (ROADMAP)
Since the debt is peripheral, it is tackled by **incremental extraction** with
identical behaviour (each step verified byte-identical), in the spirit of the
`io_device_t` work already carried out. See Epic 7 "Architectural clean-up" in
`ROADMAP`.
