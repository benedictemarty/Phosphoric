# Sprint 90 — Signal-level cassette emulation (CB1 + Timer 2)

> Status: **DONE** (v1.50.0-alpha, 2026-07-04)
> Version: 1.49.0-alpha → **1.50.0-alpha** (MINOR — new feature)
> Trigger: the game *Soccer Manager* (KnightSoft 1984) does not start
> (custom cassette loader + `EOR #$55` decryption), as on Oricutron,
> whereas it works on Euphoric and on a real machine.

## 1. Problem

Phosphoric emulates the cassette **only by patching the ROM routines**
(intercepting `getsync`/`readbyte` in `tape_patches()`, `src/main.c`).
`src/io/cassette.c::cassette_read_bit()` is a stub that returns 0.

Games with a **non-standard cassette loader** (turbo loaders, unprotection
routines, custom multi-block sequencing) bypass all or part of the
patched ROM routines → incorrect loading → crash. It is the same limit
as on Oricutron (patch-based). Euphoric and the real hardware work because they
emulate the **cassette socket at the electrical signal level**.

## 2. Real hardware mechanism (reverse engineering of ROM BASIC 1.0, verified)

ORIC cassette reading relies on **VIA CB1** (edges of the tape signal) +
**VIA Timer 2** (pulse-width measurement). ROM disassembly:

### Pulse measurement primitive `$E67D`
```
$E67D: PHA
$E67E: LDA $0300     ; reads ORB → CLEARS the CB1 flag (re-arms edge detection)
$E681: LDA $030D     ; reads IFR
$E684: AND #$10      ; bit 4 = CB1 flag
$E686: BEQ $E681     ; waits for the next CB1 edge (tape transition)
$E688: LDA $0309     ; reads Timer2 high = elapsed cycles / 256
$E68C: LDA #$FF ; STA $0309   ; reloads T2 high = $FF
$E692: CMP #$FE      ; interval < ~512 cyc → C=1 (SHORT) ; >= 512 → C=0 (LONG)
$E695: RTS           ; returns with Carry = pulse classification
```

### Bit decoder `$E661`
```
read 1 pulse; if SHORT → read 6 more (7 in total), otherwise 2 (3 in total)
count the SHORT pulses; CMP #$04: >=4 → bit '1', otherwise bit '0'
```

### Consequence for signal generation
- The CPU counts **one active CB1 edge per period** of the tape tone.
- Tone **'1' = 2400 Hz** → period ≈ 416 cycles (< 512 → SHORT).
- Tone **'0' = 1200 Hz** → period ≈ 833 cycles (>= 512 → LONG).
- ROM threshold ($FE on T2-high) ⇒ boundary ≈ 512 cycles: separates cleanly.
- Byte frame = 14 bits, LSB first: start(0) · 8 data · odd parity ·
  stop(1,1,1,1) — cf. `tap_encode_frame()` (loci_bus.c), already present.

Equivalent BASIC 1.1 (Atmos) addresses: cassette primitives around
`$E6C9`/`$E735` (to be re-checked in the disassembly when porting to the Atmos).

## 3. Design

### Signal generator (`src/io/cassette.c`)
State: tape buffer (reuses `emu->tapebuf`/`tapelen`/`tapeoffs`), position in
bits, pulse phase, cycle clock. `cassette_tick(emu, cycles)` advances
the clock and, at pulse instants, calls `via_set_cb1()` to toggle the
pin → active edge → IFR CB1 flag. The VIA Timer 2 (already modelled) measures.

Timing constants (to be calibrated empirically through an integration test):
```
CAS_PERIOD_2400 ≈ 416   /* φ2 cycles: tone '1' */
CAS_PERIOD_1200 ≈ 833   /* φ2 cycles: tone '0' */
CAS_PULSES_PER_1  = ...  /* number of 2400 Hz periods per bit '1' */
CAS_PULSES_PER_0  = ...  /* number of 1200 Hz periods per bit '0' */
CAS_LEADER        = ...  /* sync pulses (pilot tone) before each block */
```

### Motor control
Cassette motor = VIA ORB bit 6 (already snooped for LOCI, `main.c:1196`).
The signal only advances with the motor ON.

### Loop integration
Add in `cpu_cycle_tick()` (main.c:1240):
`if (emu->tape_signal_mode) cassette_tick(emu, cycles);`

### Activation
New CLI flag **`--tape-signal`**: switches to signal-level reading (disarms
the `getsync`/`readbyte` patches). Patch mode remains the default (compatibility,
speed). Incompatible with `-f` (fast-load) → clear message.

## 4. Breakdown (testable increments)

- **P1** — Skeleton: cassette state struct, timing constants, `cassette_tick`
  + `cassette_signal_*` API, `--tape-signal` flag, `via_set_cb1` wiring.
  Unit tests: 14-bit frame encoding, edge scheduling, motor gate.
- **P2** — Calibration loop: integration test "bytes → signal → real ROM
  `readbyte` routine → bytes read back"; tuning the constants until identical.
- **P3** — End-to-end loading of a standard `.tap` through the real ROM (without
  patches), verified by memory comparison (BASIC program loaded).
- **P4** — *Soccer Manager* validation (custom loader): title + game start.
- **P5** — Atmos port (ROM 1.1 addresses), docs, update of the tracking files.

## 5. Acceptance criteria

1. `--tape-signal` loads a standard BASIC `.tap` through the real ROM (ORIC-1),
   program identical byte for byte.
2. *Soccer Manager*: title screen THEN game started (no more return to BASIC).
3. All tests pass (`make tests`); no regression of patch mode.
4. Valgrind clean on the new suite.
5. Tracking files up to date (CHANGELOG, VERSION_TRACKING, CIRRUS_OS, ROADMAP).

## 6. Result (delivered)

- **Final constants**: bit '1' half-period = 208 cyc (period 416),
  high half-period of a bit '0' = 416 cyc (period 624), 1st half-period
  always 208 (anchored phase), ROM threshold ~512 cyc. Leader = 64 frames of 0x16.
- **Key discovery**: the PB6 motor line is overwritten by the keyboard scan
  (same ORB bits at rest and during CLOAD) → gating on the PC within the ROM
  read routine instead, with a rewind on first entry. Position
  preserved between bytes/blocks (signal mode "pauses" outside reading),
  which naturally handles protected multi-block loaders.
- **Verified**: `hello.tap` (standard BASIC) loaded + listed through the real ROM;
  *Soccer Manager* (ORIC-1) loaded + started ("What is your first name please ?").
- **Tests**: `make test-cassette` (8/8), full suite 876/876, valgrind 0
  errors, strict static analysis clean.
- **Not covered in v1** (backlog): fast mode (2400 baud, `zp$67`), porting the
  Atmos gating addresses (the protocol is identical; only the
  `readbyte_entry/getsync_end` bounds of `rom_patches` differ, already present).
