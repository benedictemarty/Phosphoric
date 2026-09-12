# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Phosphoric** — Bus-cycle-accurate ORIC-1/Atmos emulator written in C11 (cycle-stepped 6502 core: one bus access per cycle, NMOS dummy accesses included, IRQ/NMI sampled at the penultimate cycle; the historical padded core remains as `--cpu-legacy`). Emulates the complete ORIC 8-bit computer (1983): MOS 6502 CPU, 64KB memory with ROM/RAM banking, VIA 6522, AY-3-8910 PSG audio, ULA video (text 40x28 + HIRES 240x200), Microdisc WD1793 FDC, and cassette TAP format. Supports both ORIC-1 (BASIC 1.0) and Atmos (BASIC 1.1) with ROM auto-detection. Optional SDL2 for display/audio/input.

**Accuracy level: the CPU core is N3 (cycle-stepped, verified 100% against the 65x02
oracle); the whole machine is cycle-stepped since 2.0.0-alpha.8 (VIA timers, ULA fetch
and PSG at their documented level; FDC still N1+).** `docs/ACCURACY.md`
holds the N1→N4 scale, the per-component classification, and the only wording that
may be used publicly; `docs/specs/V2_CYCLE_ACCURACY.md` is the V2 plan that takes
CPU/VIA/ULA/PSG to N3. The accuracy claim must always carry its `bus-` qualifier —
`make test-docs-claims` fails on an unqualified one.

The measuring instruments exist (V2-S1): `make test-cycle` replays the
SingleStepTests/65x02 vectors and scores four separate properties (final state,
cycle totals, bus subsequence = N2, exact bus sequence = N3); `make test-dormann`
runs Klaus Dormann's functional test. Both SKIP when the (unvendored) vectors are
missing — `tools/fetch_vectors.sh`. When touching the CPU core, run them: the
oracle is what turns a timing claim into a number.

## Build Commands

```bash
make                     # Standard build with SDL2 (default since v1.67)
make SDL2=0              # Headless build (no SDL2, for CI/automation)
make DEBUG=1             # Debug build (-g -O0)
make CAST=1              # Build with Chromecast MJPEG streaming
make COVERAGE=1          # Build with gcov coverage instrumentation
make tools               # Build conversion tools (bas2tap, bin2tap, tap2sedoric)
make clean               # Clean build artifacts
make install PREFIX=/usr/local
```

## Testing

```bash
make tests               # All test suites (must all pass before commit)
make test-cpu            # CPU tests
make test-memory         # Memory tests
make test-io             # VIA/I/O tests
make test-storage        # Storage tests
make test-system         # Integration tests
make test-rom            # ROM compatibility tests
make test-video          # Video export tests
make test-audio          # PSG audio tests
make test-debugger       # Debugger tests
make test-savestate      # Save state tests
make test-atmos          # Atmos support tests
make test-joystick       # Joystick tests
make test-printer        # Printer tests
make test-mcp40          # MCP-40 plotter tests
make test-renderer       # Display scaling tests
make test-trace          # CPU trace logging tests
make test-clock          # Master clock (emu_cycle): one call = one machine cycle
make test-raster-split   # ULA per-cycle fetch proof (mid-line raster split)
make test-savestate-determinism  # a mid-frame savestate resumes EXACTLY (raster stops, VIA, RAM)
make test-bench          # blocking perf budget (≤ 1000 µs/frame; motivated SKIP on a throttled host)
make test-corpus         # local media replayed at fixed cycles vs tests/corpus/manifest.sha256
                         # (re-baseline on purpose: tools/corpus_replay.sh snapshot)
make test-cycle          # Cycle-by-cycle CPU conformance oracle (SingleStepTests/65x02)
make test-dormann        # Klaus Dormann 6502 functional test
make fetch-vectors       # Fetch oracle vectors (third-party, not vendored, ~1GB)
make test-profiler       # CPU profiler tests
make test-rominfo        # ROM analysis tests
make test-serial         # ACIA 6551 serial tests
make test-coverage       # Code coverage meta-tests
make test-cast           # Cast server tests (requires CAST=1 build)
make valgrind            # Memory leak detection (all suites under Valgrind)
make static-analysis     # Extra compiler warnings (-Wshadow, -Wconversion, etc.)
make coverage            # Full coverage pipeline: clean, build with gcov, run, report
```

Current test count is tracked in VERSION_TRACKING (check there for the authoritative number).

Test framework is custom C macros redefined in each test file (no shared header, no external dependency): `TEST()`, `RUN()`, `ASSERT_EQ()`, `ASSERT_TRUE()`, `ASSERT_FALSE()`. Each test file defines its own `tests_passed`/`tests_failed` counters and a `setup()` helper to initialize the relevant subsystem. To add a new test: define a `TEST(name)` function, call it via `RUN(name)` in `main()`. Tests compile and run in one step via their Makefile target.

## Architecture

### Core emulator structure: `emulator_t` (include/emulator.h)
Central struct containing all hardware subsystems. Passed as pointer to most subsystem functions. Key constants: `CYCLES_PER_FRAME = 19968` (PAL: 312 lines x 64 cycles), `ORIC_CLOCK_HZ = 1000000`, `ORIC_FRAME_RATE = 50`.

### Hardware subsystems (src/)
- **cpu/** — MOS 6502, 256/256 opcodes, 13 addressing modes, level-triggered IRQ (IRQF_VIA, IRQF_DISK).
  **Two cores sharing one semantics**: `microseq.c` (cycle-stepped, **the default**, 100%
  conformant to the 65x02 oracle, IRQ sampled at the penultimate cycle) and `opcodes.c`
  (historical, bus-cycle ordered, `--cpu-legacy`). Calculations live in `opcodes.c` only
  (`cpu_rmw_apply`, `cpu_op_adc/sbc/cmp/lax`, `cpu_sh_unstable`, `cpu_update_nz`) — never
  duplicate an operation's semantics into the sequencer, which only schedules cycles.
- **memory/** — 64KB: RAM ($0000-$BFFF), VIA I/O ($0300-$030F), Microdisc I/O ($0310-$031F), ROM/RAM overlay ($C000-$FFFF)
- **io/via6522.c** — VIA 6522: 16 registers, Timer 1/2, IFR/IER interrupts, Port A/B callbacks, keyboard matrix scanning
- **io/keyboard.c** — 8x8 matrix: VIA ORB bits 0-2 select column, Port A reads rows (active low)
- **io/joystick.c** — IJK joystick adapter: active low on PSG Port A, keyboard/gamepad modes
- **io/printer.c** — Centronics printer: VIA Port A data + CA2 STROBE, text file capture
- **io/mcp40.c** — MCP-40 4-color pen plotter: 480x400 framebuffer, Bresenham line drawing, BMP export
- **io/cassette.c** — Cassette interface: TAP format loading/saving (CLOAD/CSAVE ROM patching, post-CLOAD rechain)
- **io/acia6551.c** — ACIA 6551 serial at $031C-$031F: TX/RX, IRQ, baud rate timing, V23 mode (Digitelec DTL 2000, Minitel)
- **io/serial_backend.c** — Serial backends: loopback, TCP, PTY, modem Hayes (AT commands, 64KB buffers), COM (termios), Digitelec DTL 2000
- **io/serial_picowifi.c** — PicoWiFiModemUSB (sodiumlb): WiFi modem emulation with full v0.1.0 AT command set, exposed via LOCI as ACIA at $0380 (`--serial picowifi[:SSID[:PASS]]`)
- **io/microdisc.c** — Microdisc: WD1793 FDC at $0310-$031F, 4 drives, overlay ROM banking
- **video/** — ULA: **one 6-pixel cell fetched per cycle** (`video_line_begin` /
  `video_render_cell` / `video_line_end`; ink/paper/attributes are a per-line serial
  state in `video_t`), so a mid-line write only affects cells not yet scanned.
  `--ula-line` renders a whole scanline at once. Never re-render a frame before a
  capture in per-cycle mode — it would erase the scan (see `emu_refresh_for_capture`).
  PPM/BMP/PNG/ASCII export, `renderer.c` for SDL2 scaling (x1-x4)
- **audio/** — AY-3-8910 PSG **clocked at hardware rate** (`clock/8` = 125 kHz), output
  **integrated** over the steps each sample covers (no aliasing above Nyquist): tone
  `clock/(16·TP)`, noise LFSR `clock/(16·NP)`, envelope `clock/(8·EP)`. Verify audio by
  **measuring the signal** (frequency, envelope duration), never by diffing WAV bytes —
  a recalculation once declared the envelope conformant while it was 2x too slow.
  Register writes are CPU-cycle timestamped (digidrums). SDL2 audio callback.
- **storage/** — TAP format, Sedoric filesystem, WD1793 disk controller
- **hostfs/** — Host filesystem sharing (--hostfs DIR), VFS abstraction layer
- **utils/** — Logging, INI config parser, CPU trace, cycle trace (`--cycle-trace`,
  fed by `cpu_set_bus_callback()`), CPU profiler, ROM analysis
- **network/** — MJPEG cast server, CASTV2 Chromecast client (requires CAST=1)
- **debugger.c** — Interactive REPL: breakpoints (16 max), watchpoints (8 max), step/continue, register/memory inspection
- **savestate.c** — Binary .ost format: 10 sections (CPU, MEM, VIA, PSG, VID, KBD, FDC, MDC, TAP, META) with CRC32

### Emulation loop (src/main.c) and master clock (src/emu_clock.c)
Runs `CYCLES_PER_FRAME` (19968) cycles per frame at 50 FPS. `emulator_run()` is
126 lines that dispatch **named per-frame steps** (`run_frame_instructions`,
`run_fastload_hooks`, `run_autotype_step`, `run_present_and_events`,
`run_timed_captures`, `run_frame_pacing`, `run_end_of_run`, …) sharing a
`run_state_t` (cycles, frames, clocks). **Their order is observable** (captures,
keystrokes, pacing) — add a new hook as one more `static` step at the right place,
never inline in the loop. `run_frame_instructions` does the per-INSTRUCTION work
(debugger, trace, profiler, tape patches) and calls
`emu_step()`; the **master clock** `emu_cycle()` owns the per-CYCLE work with a
fixed intra-cycle order **measured on hardware** (Mike Brown's ULA guide): **CPU**
(its single bus access) → **φ2 peripherals** (VIA, FDC, ACIA, DTL, Mageco, cassette,
one cycle at a time — never a batch) → **ULA** (fetch of the cell of the same count,
raster advance, due scanlines, ULA-NG tick). A CPU write at cycle c is seen by cell c. Never compute
a raster position in the loop again: ask `emu_raster_pos()`. **Every `emu_cycle()`
call must cost the CPU exactly one bus cycle** — a micro-op returning without a bus
access desyncs the ULA from CPU/VIA while the oracle stays green (`test-clock` guards
it). The loop follows `emu->raster_cycle`; a savestate restores it (section `CLK`)
and `emu_clock_resume()` picks the frame up mid-way. Contract in
`docs/architecture/master-clock.md`.

### I/O routing
Memory reads/writes in the I/O range trigger `io_read_callback()`/`io_write_callback()` which route to:
- **$0300-$030F** → VIA 6522
- **$0310-$031B** → Microdisc WD1793 FDC
- **$031C-$031F** → ACIA 6551 serial (configurable base via `--acia-addr`)

Callbacks are registered via `memory_set_io_callbacks()` in `main.c`.

### Tape loading flow
ROM cassette routines (CLOAD/CSAVE) are intercepted by PC-matching patches (`rom_patches_t` in `emulator.h`). Patch addresses differ between BASIC 1.0 and 1.1 — selected at boot via ROM auto-detection. Post-CLOAD, BASIC line pointers are rechained (`cassette_rechain_basic()`) to fix link addresses.

### Tools (tools/)
- `bas2tap` — BASIC text → .TAP
- `bin2tap` — Binary → .TAP with load/exec address
- `tap2sedoric` — .TAP → Sedoric disk

## Key Conventions

- **Language:** C11, gcc >= 9.0 or clang >= 10.0
- **Style:** 4-space indent, K&R braces, `snake_case` functions/vars, `UPPER_CASE` macros, 100 char line limit
- **Headers:** Include guards `#ifndef FILE_H`
- **Commits:** Conventional commits (feat:, fix:, docs:, test:)
- **Versioning:** Semantic (MAJOR.MINOR.PATCH-LABEL), current version in `EMU_VERSION` macro in `include/emulator.h`

## Per-Commit Requirements

Every modification must:
1. Run `make tests` — all tests must pass
2. Update **CHANGELOG** — Keep a Changelog format, entries under `## [version] - date` with `### Added/Fixed/Changed` subsections
3. Update **VERSION_TRACKING** — version, date, status line, total test count
4. Update **CIRRUS_OS** — build/test status summary, component checklist `[V]`/`[ ]`
5. Update **ROADMAP** — sprint progress and task completion

The current version string is defined in `EMU_VERSION` macro in `include/emulator.h`. Keep it consistent across all tracking files.

## Running the Emulator

```bash
# Basics
./oric1-emu -r roms/basic10.rom                          # Boot BASIC 1.0 (ORIC-1)
./oric1-emu -r roms/basic11b.rom                         # Boot BASIC 1.1 (Atmos)
./oric1-emu -r roms/basic10.rom -t prog.tap -f           # Fast-load tape
./oric1-emu -r roms/basic10.rom --disk-rom roms/microdis.rom -d SEDO40u.DSK  # Sedoric

# Serial backends (--serial TYPE)
# loopback, tcp:host:port, pty, modem[:host:port], modem:listen:port,
# com:baud,bits,parity,stop,device, digitelec:host:port,
# picowifi[:SSID[:PASS]]  (LOCI WiFi modem, ACIA $0380 under --loci)
# Options: --acia-addr XXXX, --serial-v23, --serial-buffer N,
#          --serial-irq-on-rdrf, --serial-trace FILE

# Debugging
./oric1-emu -r roms/basic10.rom --debug                  # Start in debugger
./oric1-emu -r roms/basic10.rom --trace trace.log        # CPU instruction trace
./oric1-emu -r roms/basic10.rom --profile prof.txt       # CPU profiler
./oric1-emu -r roms/basic10.rom --rom-info               # ROM analysis
```

See `./oric1-emu --help` or README.md for the full CLI reference.

## Dependencies

- **Required:** GCC/Clang, Make, libm
- **Optional:** SDL2 (display/audio/input), pkg-config, Valgrind, OpenSSL (for CAST=1)
