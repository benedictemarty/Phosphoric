# Phosphoric V2 — Real cycle accuracy

**Status**: plan approved for execution · **Created**: 2026-09-10
**Base**: 1.120.0-alpha · **Target**: 2.0.0
**Method**: Scrum, 2-week sprints, long-lived branch `v2/cycle-accuracy`
**Prerequisite reading**: [docs/ACCURACY.md](../ACCURACY.md) (N1→N4 scale, current ranking)

---

## 1. Why a V2

The project advertised itself as accurate to the cycle. The 2026-09-10 audit
establishes that the core is **N2** (ordered at bus-cycle level) and that video,
PSG and FDC are **N1**. The gap is not cosmetic: it makes unreachable the effects
that give a reference Oric emulator its value (mid-line raster splits,
attributes switched during the scan, timer-driven PSG sample players,
signal-level tape loading). Fixing it touches the scheduling of **all**
components: it is an architectural break, hence a V2 and not a series of
1.x fixes.

**V2 objective**: bring CPU, VIA, ULA and PSG to level **N3**
(cycle-stepped, in lockstep), and prove it with an external oracle harness
rather than by assertion.

**Non-goals** (remain outside the V2 scope): Telestrat, generalised N4
(only the expansion bus stays sub-cycle, via the existing Epic B), generalised
bit-level MFM model of the FDC (handled as an option, not as the default).

---

## 2. Target architecture

### 2.1 Master clock

Today the CPU is the master of time: `cpu_tick()` fires an `on_cycle`
callback per bus access, then `cpu_step()` **pads** the internal cycles in
a single block at the end of the instruction, and `cpu_cycle_tick()` propagates
that block to the peripherals. Consequence: internal cycles are invisible to the
bus, and the VIA's IFR can be set up to 6 cycles too late inside an
instruction.

Target: a single function

```c
void emu_cycle(emulator_t* emu);   /* advances the WHOLE machine by one cycle */
```

with a fixed and documented intra-cycle order:

1. **φ1** — the ULA performs its memory access (character / pattern / attribute fetch).
2. **φ2** — the CPU executes *its* single cycle action (real bus access,
   dummy access, or explicit internal cycle).
3. **end of cycle** — VIA, ACIA, FDC, PSG, DTL, Mageco evaluate their edges
   over exactly 1 cycle; the interrupt lines are updated.
4. **sampling** — the state of the IRQ/NMI lines is latched for the CPU's
   decision at the next cycle (penultimate model).

`cpu_step()` becomes a simple `while (!cpu_instruction_done) emu_cycle()`,
kept for the debugger, the tests and the existing API.

### 2.2 Micro-sequenced 6502 core

Each opcode becomes a **sequence of micro-operations** (one per cycle),
either through a micro-op table or through a state machine per addressing mode.
Each cycle carries: access type (read / write / internal), address
emitted, effect on the registers. Dummy accesses become first-class cycles
and are therefore **visible to the peripherals**, which is the very
definition of N3.

### 2.3 Compatibility

- Cycle totals per opcode do not change → the 1001 existing tests
  remain non-regression constraint no. 1.
- The `.ost` (savestate) format evolves: resuming must be possible **in the
  middle of an instruction**. New `CPUµ` section (micro-op index,
  latent registers, interrupt latches), format version bumped, reading
  of 1.x `.ost` files preserved (resume at an instruction boundary).

---

## 3. Epics

| # | Epic | Priority | Sprints | Version |
|---|------|----------|---------|---------|
| **V2-E0** | Truthful communication + oracle harness | Critical | S0-S1 | 1.121.0 |
| **V2-E1** | Micro-sequenced 6502 core (N3) | Critical | S2-S4 | 2.0.0-alpha.1 |
| **V2-E2** | Master clock & scheduling | Critical | S4-S5 | 2.0.0-alpha.2 |
| **V2-E3** | VIA 6522 per cycle | High | S5-S6 | 2.0.0-alpha.3 |
| **V2-E4** | Video ULA per cycle (byte fetch per cycle) | High | S6-S8 | 2.0.0-alpha.4 |
| **V2-E5** | PSG AY-3-8910 at `clock/16` | High | S8-S9 | 2.0.0-alpha.5 |
| **V2-E6** | Cassette & FDC derived from real time | Medium | S9-S10 | 2.0.0-beta.1 |
| **V2-E7** | Perf, savestate, non-regression, CI | Critical | cross-cutting | 2.0.0-rc |
| **V2-E8** | Documentation, communication, release | High | S11 | 2.0.0 |

---

### V2-E0 — Truthful communication + oracle harness

Nothing is changed in the core until the oracle exists: it is what turns
"I believe it is right" into "it is measured".

- **US0.1 — Rewording of the claims.** `README.md`, the project's local rules,
  `ROADMAP`, `docs/AGILE_PLAN.md`, `cpu6502.h` headers, distribution pages,
  forum notes: apply the permitted vocabulary of `docs/ACCURACY.md`.
  Remove the unqualified cycle-accuracy tick ("timing" item) from the *Success Metrics* and
  replace it with "N2 ordered at bus-cycle level (N3 targeted in 2.0.0)".
  *Acceptance*: `grep -ri "cycle.accurate"` only returns qualified
  occurrences; a guard test (`test_docs_claims.sh`) fails if a bare
  occurrence reappears.
- **US0.2 — External CPU oracle. ✅ delivered (v1.122.0-alpha)** — the
  **SingleStepTests/65x02** vectors (10,000 cases per opcode, each with the initial state,
  the final state and the expected cycle-by-cycle bus trace), fetched outside the repository
  by `tools/fetch_vectors.sh` / `make fetch-vectors` (~1 GB, not versioned).
  Runner `tests/unit/test_cpu_cycles.c` → `make test-cycle`: home-made JSON
  parser (no dependency), **flat 64 KB** test machine (`rom_enabled=0`
  + trivial I/O callbacks so that `$0300-$03FF` and `$C000-$FFFF` are
  neither swallowed by the I/O bus nor read-only), and **four properties measured
  separately**: final state, total cycles, bus **subsequence** (= the
  N2 property), **exact bus sequence** (= the N3 property). Clean SKIP without
  vectors, and the parser remains tested on an embedded case. The 12 JAM opcodes
  are counted separately (modelling divergence, off the scale: the real NMOS
  locks the bus, Phosphoric halts the CPU).
  *Acceptance*: met — score published, and `BUS_EXACT_FLOOR_BP` floor locked
  against regressions, to be raised by V2-E1.
- **US0.3 — Klaus Dormann functional test. ✅ delivered (v1.122.0-alpha)** —
  `tests/unit/test_dormann.c` → `make test-dormann`: `6502_functional_test.bin`
  loaded into a flat 64 KB, started at `$0400`, run until the `jmp *`. **It passes
  in full** (success trap `$3469`, ~96 M emulated cycles in 0.7 s of host
  time) — the core is therefore functionally sound, its shortcoming is indeed temporal
  and not logical. The decimal test is only published as `.a65` source (no upstream
  binary): assembling it would require `as65`, out of scope — the functional test
  already covers decimal mode.
- **US0.4 — Bus trace. ✅ delivered (v1.122.0-alpha)** — new hook
  `cpu_set_bus_callback()` (one notification per real bus access, shared with
  the oracle), module `src/utils/cycle_trace.c`, options `--cycle-trace FICHIER`
  and `--cycle-trace-max N`. One line per cycle: `cycle, type (R/W/i), adresse,
  donnée, PC, A X Y SP P, drapeaux, ligne d'IRQ` (cycle, type, address, data, PC,
  registers, flags, IRQ line). The `i` lines are the padded internal cycles of N2
  (without address); they will carry their real access after
  V2-E1 — the trace thus makes the shortcoming **visible to the naked eye** as of today.

### V2-E1 — Micro-sequenced 6502 core

- **US1.1 — Micro-sequencer skeleton. ✅ delivered (v1.123.0-alpha)** —
  `src/cpu/microseq.c`: each instruction is broken down into a **plan of
  micro-operations, one per cycle**; `cpu_cycle()` executes exactly one of them and
  `cpu_step()` is now merely a wrapper. All 256 opcodes are covered, classified
  into 17 sequence families; the addressing MODE comes from `opcode_table` (single
  source) and the SEMANTICS from the shared functions of `opcodes.c`
  (`cpu_rmw_apply`, `cpu_op_adc/sbc/cmp/lax`, `cpu_sh_unstable`, `cpu_update_nz`)
  — the two engines differ **only** in scheduling, never in
  computation. **Opt-in** engine (`--cpu-microseq`) for the duration of the migration.
  *Acceptance met*: identical cycle totals (Dormann succeeds at the very same
  cycle: 96,241,367 on both engines), `make tests` unchanged.
- **US1.2 — Dummy accesses. ✅ delivered (v1.123.0-alpha)** — all present in the
  plans: read at the uncorrected address (on page crossing for
  reads, **always** for writes and RMW), read of the base before
  indexing in `zp,X`/`zp,Y` and `(zp,X)`, RMW write-back, dead stack reads
  (`PLA`/`PLP`/`JSR`/`RTS`/`RTI`), dead read of implied instructions, branch
  re-fetch and page-correction cycle. *Acceptance exceeded*:
  **100.00 %** exact bus sequence over **2,440,000 cases** (the objective was
  "≥ 99.9 %"), plus unit tests showing the dummy access observed
  on the bus and the absence of that same access on the historical engine (`test-cpu`).
- **US1.3 — Interrupts at the right cycle. ✅ delivered (v1.124.0-alpha)** —
  /IRQ and /NMI sampled **at every cycle**, the end-of-instruction decision
  being based on the sample of the **penultimate cycle**
  (`ms_irq_sampled`/`ms_nmi_sampled`). The I mask is taken into account **at the
  moment of sampling**, which yields "for free" the delayed semantics
  of `CLI`/`SEI`/`PLP` (they modify I on their last cycle). NMI edge latch
  kept; **NMI hijacking** if /NMI falls before the cycle that pushes P in a
  `BRK` or an IRQ sequence. *Acceptance met*: 7 dedicated unit tests
  (IRQ armed on the last cycle seen too late + its counter-test, `SEI` that does not
  protect, `CLI` and `PLP` that delay, NMI/BRK hijacking, interrupt
  sequence = 7 cycles all carrying an access); ORIC-1/Atmos boots and
  **13 corpus programs** (6 disks + 7 tapes) identical.
  *Fixed along the way* a defect introduced in V2-S2: the interrupt
  sequence lasted 8 cycles instead of 7 (one `M_DUMMY_PC` too many) — invisible
  on screen, but wrong; found by the "7 cycles" test.
- **US1.4 — Padding removed, default engine. ✅ delivered (v1.124.0-alpha)** —
  the micro-sequencer becomes the **default** engine (`cpu_init`), so there are no
  more padded cycles on the normal path: every cycle carries its access. The historical
  engine remains available through `--cpu-legacy` (and `--cpu-microseq` is kept
  as a no-op for existing scripts). *Acceptance met*: `make tests`
  fully green with the new default.

### V2-E2 — Master clock & scheduling

- **US2.1 — `emu_cycle()`. ✅ delivered (2.0.0-alpha.1)** — `src/emu_clock.c`:
  single entry point for time, with the intra-cycle order **fixed and documented**
  (φ1 ULA → φ2 CPU → φ2 peripherals). The beam counters
  (`raster_cycle`, `raster_rendered`, `raster_ng_line`, `raster_next_line`),
  until then local variables of the main loop, now live in
  `emulator_t`: any caller can clock the machine.
  `emu_step()` wraps `emu_cycle()`; `emu_raster_pos()` gives the beam
  position (PAL line 0-311, cycle within the line 0-63) — the basis of the per-cycle
  fetch of epic E4. Full contract in
  [docs/architecture/master-clock.md](../architecture/master-clock.md).
- **US2.2 — Removal of batches. ✅ delivered (achieved by US1.4, verified here)** —
  with the micro-sequenced core, every cycle is a bus access: the clock
  callback **always receives `cycles = 1`**, so `via_update`, `io_bus_tick`,
  `fdc_ticktock` and `cassette_tick` advance one cycle at a time. Measured:
  40 calls for 40 cycles, at most 1 cycle per call
  (`test_peripherals_get_one_cycle_at_a_time`) — the test would fail if a batch
  reappeared.
- **US2.3 — Intra-cycle order documented and tested. ✅ delivered (2.0.0-alpha.1)** —
  `test_ula_reads_before_cpu_writes` sets up the edge case: a CPU write
  landing **exactly** on the cycle where scanline 0 is emitted is not visible
  in that line, but is in the next one. This is the hardware's visibility
  convention (the ULA accesses RAM in φ1, the CPU in φ2).
  New suite `make test-clock` (8 tests).
- **US2.4 — Junction with Epic B. ✅ delivered (documentation)** — the 30 φ2 sub-ticks of the
  expansion bus (`include/io/bus_timing.h`) are now explicitly
  described as a **subdivision of the φ2 phase** of `emu_cycle()`; the
  motherboard peripherals always win the race, their cost remains zero.

### V2-E3 — VIA 6522 per cycle

- **US3.1 — One-cycle step. ✅ delivered (achieved in 2.0.0-alpha.1)** — `via_update()`
  always receives `cycles = 1` since the switch to the master clock, and the
  timer countdown is now done cycle by cycle within the function.
- **US3.2 — Exact edge timings. ✅ delivered for the timers (2.0.0-alpha.2)** —
  underflow is no longer reaching zero but the transition
  `$0000 → $FFFF`, followed by a **reload cycle** (`t1_reload`): the period
  of continuous mode becomes **N+2**, as per the datasheet, whereas the code gave
  **N** — 0.02 % error at 100 Hz but **20 % for N=10**, audible on short sounds
  and digidrums. Same mechanism for Timer 2. Also verified: counter
  that keeps counting down after a one-shot time-out without re-arming, PB7
  square wave (edge every N+2 cycles), counter read-back and flag clearing by
  T1C-L but not by T1C-H, CA2 pulse of exactly one cycle.
  *Still outside the model*: the **half-cycle** of the one-shot time-out (N+1.5 → set to
  N+1), not representable at whole-cycle resolution.
- **US3.3 — Timing vectors. ✅ delivered (2.0.0-alpha.2)** — 7 tests in
  `test-io` (46 → **53**), including the integration test **"Timer 1 continuous at the frame
  period: exactly 50 interrupts in 50 frames"**, which fails for a single
  cycle of drift.
- **US3.4 — Revisiting the accepted deviations. ✅ delivered (2.0.0-alpha.2)** — deviation
  no. 2 of `docs/HARDWARE_CONFORMANCE.md` §2 ("period ≈ N+1") is
  **lifted**, and the reason that justified it ("touching the countdown would shift
  all byte-exact baselines") turned out to be **unfounded**: corpus and suites
  intact. What made the fix safe is that the machine now advances
  cycle by cycle (E1/E2). Deviations 4 (effect of the T1L-H write
  on the flag, conflicting datasheets) and 5 (RESET clearing the counters)
  remain accepted, for lack of a reliable source — one does not fill an uncertainty with
  an invention.

### V2-E4 — Video ULA per cycle

The most visible gain for the user.

- **US4.1 — Beam model. ✅ partially delivered** — the beam position
  (PAL line 0-311, cycle within the line 0-63) is exposed by `emu_raster_pos()`
  from the master clock, and it drives the fetch. **Not delivered**:
  rendering of the border and blanking areas (the image remains 240×224, the
  88 vertical blanking lines are not painted) — to be handled if a real need
  arises (overscan).
- **US4.2 — Byte fetch per cycle. ✅ delivered (2.0.0-alpha.3)** — rendering is
  split into `video_line_begin()` / `video_render_cell()` / `video_line_end()`;
  ink, paper, text attributes and fine scroll become a **line
  state** carried by `video_t`, which makes cell-by-cell rendering strictly
  equivalent to line-by-line rendering when memory does not change. The clock
  calls one cell per cycle in phase φ1. **Default** since this version;
  `--ula-line` restores the old behaviour, and `--cpu-legacy` forces it (an
  instruction is indivisible there). *Consequence fixed along the way*: the screen
  capture re-rendered the whole frame in one block, which **erased** the result of
  the scan — it now takes the framebuffer as is.
  *Accepted caveat*: the cycle at which column 0 is fetched is not calibrated
  against real hardware (`--ula-fetch-offset`, default 0) — the same
  approach as the unmeasured constants of epic B.
- **US4.3 — Modes. ✅ delivered** — TEXT 40×28, HIRES, text footer 200-223,
  inversion, blinking, double height, serial attributes and ULA-NG (start
  address, fine scroll, parallel attributes) all go through the per-cell
  path. The **full-screen** ULA-NG modes (chunky 4bpp, 80-column text) and
  sprite composition remain rendered in one block at the end of the line: they are not
  original hardware, and their pipeline is not serial.
- **US4.4 — Reference corpus. ✅ delivered (2.0.0-alpha.3)** — new
  `make test-raster-split`: a 6502 program (32 bytes, hand-assembled in
  the test) rewrites the screen in a loop during the scan; the test compares the two
  renderings and requires **partially** different lines — a difference that
  covers only part of the width proves that sampling is intra-line.
  Measured: **57 lines** in this case. The deterministic and exact case is in
  `make test-clock` (`test_ula_per_cycle_mid_line_split` and its counter-test
  `test_line_render_cannot_split`), plus strict equivalence on a static screen
  (byte-identical framebuffer between the two paths over a whole frame).
  **13 corpus programs** (6 disks, 7 tapes) give identical PNG captures
  between the two modes.

### V2-E5 — PSG AY-3-8910 at `clock/16`

- **US5.1 — Hardware clocking. ✅ delivered (2.0.0-alpha.4)** — the machine runs at
  **`clock/8` = 125 kHz**, the chip's actual internal step (the datasheet's `/16`
  applies to the period, and the square output toggles twice per period). Tone
  `clock/(16·TP)`, 17-bit noise LFSR `clock/(16·NP)`, envelope
  **`clock/(8·EP)`** — i.e. a 32-state cycle at `clock/(256·EP)`, the datasheet
  formula. **The envelope was twice too slow**, and yet the conformance
  document declared it compliant: its recalculation assumed a cycle of
  16 states instead of 32. An error of assumption that no review would have
  caught — only **measuring the signal** revealed it.
- **US5.2 — Resampling. ✅ delivered (2.0.0-alpha.4)** — each output sample
  **integrates** the output over the clock steps it covers (box filter,
  Q16 accumulator of 2.834 steps per sample on the ORIC). Measured consequence:
  a tone at TP=1 (62.5 kHz) used to come out **aliased at 18.4 kHz at full
  amplitude**; it is now attenuated (RMS divided by ~3), as the
  loudspeaker of a real machine would do.
- **US5.3 — Audio non-regression. ✅ delivered (2.0.0-alpha.4)** — verification is
  **spectral**, not byte by byte: tone frequency compared with `clock/(16·TP)`
  for 7 periods (±0.5 %, measured ±0.1 %), envelope duration compared with
  `clock/(8·EP)` for 4 periods (±2 %), attenuation above Nyquist, LFSR
  never stuck at zero and balanced output. `test-audio` 13 → **17**. No reference
  WAV needed re-baselining: the existing audio suites
  (`test-audio-capture`, AVI, cast) pass unchanged.
- **US5.4 — Digidrums. ✅ kept** — the timestamped path (`ay_write_data_timed`,
  event queue replayed at the exact sample position) is unchanged and
  benefits directly from the hardware clocking.

- **US5.5 — Output stage (added afterwards, on request). ✅ delivered
  (2.0.0-alpha.5)** — rather than leave two "accepted deviations" resting
  on an uncertainty, the **official schematic** was studied
  (`docs/architecture/oric-audio-output.md`). Result: the parallel mixing
  **averages** the channels — our `somme/3` (sum/3) was therefore **already correct**, the deviation
  was wrong; the circuit's only low-pass filter cuts at **37 kHz**, hence out of band;
  and the capacitive coupling **blocks DC**, which we did not do. Fixed:
  DC **+8188 → +15**, symmetric signal. Still undetermined and documented: the
  exact cutoff of the coupling (value marked "2k2" without a unit — 2.2 nF ⇒ 4.7 kHz or
  2.2 µF ⇒ 4.7 Hz, a factor of one thousand). `test-audio` 17 → **20**.

**Cost: zero.** Measured in the same session against the previous sprint's binary
(3 passes, with and without audio generation): 611 µs versus 614 µs per frame, i.e.
the equivalent of measurement noise. The apparent differences between sprints came from
the machine's variability (±5 %), not from the code.

### V2-E6 — Cassette & FDC derived from real time

- **US6.1 — Signal-level tape by default. ❌ not adopted, based on measurement** — signal
  mode exists (`--tape-signal`), works and remains **essential** for
  custom loaders and protections; but making it the **default** would degrade
  usage without gaining any useful fidelity: the loaded content is identical to
  fast-load (verified in memory), at a cost of **60 M cycles instead of 25 M**
  (the real time of an actual tape, which is precisely the point… and the
  problem). Decision: fast-load remains the default path, signal mode
  remains explicit.
  *Defect identified along the way, **resolved** (2.0.0-alpha.7)*: the "Errors found"
  displayed by the Atmos after a signal-level `CLOAD` came from an **even frame
  parity instead of odd** in `cassette_encode_frame()`. ROM 1.0 does not check
  parity, ROM 1.1 does — hence a correct load followed by an error
  message. The code, its comment ("odd parity") **and** the unit test
  (named `test_encode_frame_odd_parity`) were consistent in the **same
  error**: only the hardware could settle it. `tap2wav`, which shares this
  encoder, was therefore sending wrong parity to **real machines**. New
  `make test-tape-signal`: the signal-level CLOAD path was covered by no
  test (`test_tape_roundtrip` reloads with fast-load).
- **US6.2 — Delays derived from rotation. ✅ essentially delivered
  (2.0.0-alpha.6)** — real rotational latency was already the **default**
  (`--fdc-timing real`), and the inter-byte rate was already exact (32 cycles =
  32 µs at 250 kbit/s). This sprint adds what was really missing:
  **`LOST DATA` (S2)** reported when a DRQ is not served within the time of one
  byte, **write protection (S6)** — write commands refused, bit 6
  in the status, derived from a read-only `.dsk` file or forced by
  `--disk-write-protect` — and the **terminal RNF** of multi-sector commands.
  `test-storage` 26 → **31**, no false positive on the corpus's 6 disks.
  *Accepted limitation*: the byte is not **actually** lost (the flat image
  model has no continuous MFM stream); the bit is reported at the right moment, the
  data remain intact. Scrolling the stream requires US6.3.
- **US6.3 — Optional MFM track. ⏸ backlog** — this is what would allow
  real byte loss, `READ TRACK` and `CRC ERROR`. The foundation exists (the `Write Track`
  MFM parser, the `dsk2hfe` encoder), but the value remains low for the ORIC
  (few stream-level protections) compared with the risk to disk loading.

### V2-E7 — Perf, savestate, non-regression (cross-cutting)

- **US7.1 — Performance budget. ✅ delivered (2.0.0-alpha.8)** —
  `make test-bench` (`tools/bench_check.sh`) decides: BASIC boot **≤ 1000 µs
  per frame (5 % of the 20 ms budget)**, best of 3 runs, in `make tests`.
  Tracking: 491 µs (E1) → 521 (E2) → 555 (E3) → 601 (E4) → 611 (E5).
  *Accepted limitation*: the measurement depends on the host machine — on battery at 1.2 GHz
  the same binary gives 1010-1050 µs (and so does the alpha.3 one, under the
  same conditions). The script detects a throttled machine ("low-power" profile,
  battery, frequency below half of the maximum) and then returns a **justified
  SKIP**, not a FAIL; `BENCH_STRICT=1` forces the verdict (CI: ceiling raised
  to 2000 µs on a shared runner). Lead if the budget gets tight: an event
  scheduler for the VIA timers.
- **US7.2 — Savestate as an exact resume point. ✅ delivered (2.0.0-alpha.8)** —
  reworded: savestates are taken at instruction boundaries, so "resume
  in the middle of an instruction" was moot. What was really missing: the
  **beam position** (new `CLK` section), the per-cycle state of the VIA
  (`t1_active`, `t1_reload`, pins, shift register), the interrupt sample
  of the penultimate cycle, the PSG phase. Sections extended **at the
  tail**, earlier `.ost` files read by section size.
  `emu_clock_resume()` resumes the frame at the restored position.
  `make test-savestate-determinism`: identical raster stops, VIA and RAM
  between a continuous run and a resumed run. *Found along the way*: the **phantom cycle of the
  untaken branch** (ULA ~410 cycles per frame ahead of the CPU),
  fixed and locked down in `test-clock`.
- **US7.3 — Non-regression corpus. ✅ delivered on the local corpus
  (2.0.0-alpha.8)** — `tools/corpus_replay.sh snapshot|check` +
  `make test-corpus`: each local medium replayed for a fixed number of cycles,
  screen fingerprint compared with the versioned manifest
  `tests/corpus/manifest.sha256` (media not versioned: absent = SKIP).
  *Limitation*: the 41-program OricProgramsLib corpus is not available
  on the development machine; the manifest covers 24 tapes and
  12 disks. What it proves: at equal cycle count, the same image as the
  baseline — not that the image is the hardware's.
- **US7.4 — CI. ✅ delivered (2.0.0-alpha.8)** — `.github/workflows/linux-ci.yml`:
  `SDL2=0/1` builds, `make tests`, Dormann + 65x02 sample (20 opcodes
  where the historical core differs: `make test-cycle` decides there without the full
  GB), strict `test-bench`, Valgrind on the core suites. ROMs are not
  versioned: the tests that depend on them report SKIP in CI.

### V2-E8 — Documentation, communication, release

- **US8.1 — ✅ (2.0.0-beta.1)** `docs/ACCURACY.md`: "final V2 status" summary
  (each claim backed by the test that would disprove it) + explicit limitations;
  "N3 reached" for the CPU, "machine clocked per cycle" for the whole, not
  "exact to the cycle" (FDC N1+, ULA alignment).
- **US8.2 — ✅ (2.0.0-beta.1)** `master-clock.md` (S4/S9), the project's local rules (S9),
  `README.md` (S10: header, badge, savestates, test targets).
- **US8.3 — ✅ (2.0.0-beta.1)** `docs/articles/v2-cycle-stepped.md`: what we
  had said and why it was wrong, what V2 visibly changes, the two
  defects that only another method could reveal, what we still do not claim.
- **US8.4 — ✅ (2.0.0)** tag `v2.0.0` on the four remotes, GitHub release
  (stripped Linux binary + Windows zip from the workflow); `gh-pages` WASM page
  rebuilt and published in 2.0.0 (2.0.1: wasm build repaired).

---

## 4. Sprint sequence

| Sprint | Content | Output |
|--------|---------|--------|
| **V2-S0** | US0.1 (truthful communication) + `docs/ACCURACY.md` + anti-relapse guard | 1.121.0-alpha |
| **V2-S1** | US0.2/0.3/0.4 — 65x02 oracle, Dormann, `--cycle-trace`, **baseline score published** | 1.122.0-alpha |
| **V2-S2** | US1.1 + US1.2 — micro-sequencer **and** dummy accesses: 100 % in one go (the oracle made it possible to go further than planned) | 1.123.0-alpha |
| **V2-S3** | US1.3 + US1.4 — interrupts at the penultimate cycle, delayed I flag, NMI/BRK hijacking, **switch of the default engine** → **Epic V2-E1 complete** | 1.124.0-alpha |
| **V2-S4** | US2.1 + US2.2 + US2.3 + US2.4 — master clock, end of batches, intra-cycle order tested → **Epic V2-E2 complete** | 2.0.0-alpha.1 |
| **V2-S5** | US3.1 to 3.4 — VIA 6522: exact underflow and N+2 period → **Epic V2-E3 complete** | 2.0.0-alpha.2 |
| **V2-S6** | US4.1 to 4.4 — ULA: one cell fetched per cycle, raster splits → **Epic V2-E4 complete** | 2.0.0-alpha.3 |
| **V2-S7** | US5.1 to 5.4 — PSG clocked like the hardware (`clock/8`), envelope fixed, anti-aliasing → **Epic V2-E5 complete** | 2.0.0-alpha.4 |
| **V2-S8** | Audio output stage from the schematic (alpha.5); US6.2 FDC: LOST DATA, write-protect, terminal RNF (alpha.6); US6.1 rejected based on measurement, cassette parity fixed (alpha.7) → **Epic V2-E6 complete** (US6.3 in backlog) | 2.0.0-alpha.5 → alpha.7 |
| **V2-S9** | US7.1 blocking budget + US7.2 savestate as exact resume point + US7.3 corpus + US7.4 CI; **phantom cycle of the untaken branch fixed** (the beam gained 410 cycles/frame over the CPU) → **Epic V2-E7 delivered** (US7.3 on the local corpus, not the 41 from OricProgramsLib) | 2.0.0-alpha.8 |
| **V2-S10** | Epic E8: `docs/ACCURACY.md` in final state, public technical note (`docs/articles/v2-cycle-stepped.md`), `README` — **US8.1/8.2/8.3 delivered** | 2.0.0-beta.1 |
| **V2-S11** | Release **2.0.0 published** (tag, GitHub Linux + Windows release, CI green after two build/test fixes); WASM page in backlog (emsdk) | 2.0.0 |

Vocabulary switch milestone: **reached for the CPU at the end of V2-S3**
("CPU core exact to the cycle, verified against the 65x02 oracle"). For the whole
machine, the end of V2-S8 allows **"machine clocked per cycle"** (each
component advances one cycle at a time, CPU/VIA/ULA/PSG exact at their
documented level) — not "exact to the cycle" without qualification: the FDC remains N1+ (flat-rate
DRQ/INTRQ delays) and the ULA's horizontal alignment is not calibrated. See
`docs/ACCURACY.md` § Permitted wording.

---

## 5. Risks and mitigations

| Risk | Impact | Mitigation |
|--------|--------|--------|
| Silent regression on the game corpus | High | US7.3 systematically at the end of each epic; before/after captures diffed. |
| Performance loss | Medium | US7.1 blocking; micro-sequencer as a flat table, not function pointers per cycle. |
| Cost explosion of the per-cycle ULA | Medium | Lazy rendering: only recompute a portion of a line if screen memory or ULA state has changed since the last fetch. |
| PPM/WAV references to re-baseline en masse | Medium | Each re-baseline is a separate, justified commit, with the before/after image attached to the sprint report. |
| Long-lived branch diverging from `main` | Medium | Weekly rebase; epics E0 and E8 live directly on `main`. |
| User expectation: "V2 = everything changes" | Low | US8.3 communication: V2 changes **fidelity**, not the CLI; `.ost` compatibility ensured. |

---

## 6. Definition of Done (per user story)

1. `make clean && make tests` — 100 % green, number of tests increased or justified.
2. `make test-cycle` — score increasing or stable, never decreasing.
3. `make bench` — within the US7.1 budget.
4. CHANGELOG, VERSION_TRACKING, CIRRUS_OS, ROADMAP updated.
5. Documentation of the affected component up to date (including `docs/ACCURACY.md` if a
   component's level changes).
6. Conventional commit, pushed to the 4 mirrors.
