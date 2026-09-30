# Phosphoric timing-accuracy levels — current status

**First version**: 2026-09-10 (1.120.0-alpha, audit) · **Current status**:
2026-09-11, 2.0.0 (V2 complete, epics E0 to E8 delivered)

This document exists because the project advertised itself as accurate to the
cycle while the implementation was not, in the strict sense of the term. It sets
a verifiable vocabulary, ranks each component, and served as the acceptance
reference for the [V2](specs/V2_CYCLE_ACCURACY.md) plan. It is kept **with its
history**: the quantified starting point (44.26 %) and the milestones remain
readable, because it is the trajectory that makes the result credible.

## Summary (final V2 status)

| What is true | Evidence that would disprove it |
|---|---|
| The **CPU** is exact to the cycle: one bus access per cycle, NMOS dummy accesses included, interrupts at the penultimate cycle — **100.00 %** exact bus sequence over 2,440,000 cases | `make test-cycle CYCLE_MAX_CASES=0`, `make test-dormann` |
| The **machine** is clocked per cycle: `emu_cycle()` advances the ULA, CPU and peripherals by exactly one cycle, **never idle** | `make test-clock` (14 tests, including CPU counter = raster position over one frame) |
| The **VIA** counts per cycle: underflow `$0000 → $FFFF`, period N+2, PB7, CA2 | `make test-io` (timing vectors) |
| The **ULA** fetches one 6-pixel cell per cycle, after the CPU access of the same cycle (order and reference measured by Mike Brown): a write at cycle *c* reaches cell *c* and the following ones | `make test-raster-split`, `make test-clock` |
| The **PSG** runs at `clock/8`, integrated output (no aliasing), output stage taken from the schematic | `make test-audio` (signal measurement: frequencies, envelope, LFSR) |
| A **savestate** is an exact resume point, even when taken mid-frame | `make test-savestate-determinism` |
| At equal cycle count, the local **corpus** produces the same image | `make test-corpus` |

What is **not** true, and is therefore not claimed: "the emulator is exact to the
cycle". The FDC remains N1+ (flat-rate DRQ/INTRQ delays, flat image), the
half-cycle of the VIA one-shot is not represented, the default cassette mode
remains the ROM patch, and the absolute raster/CPU phase is not modelled — it
is unobservable on an unmodified ORIC (VSYNC hack not emulated). Component-by-component details below.

## Reference scale

| Level | Name | Operational definition | Test that proves it |
|--------|-----|---------------------------|--------------------|
| **N1** | Counted per instruction | The total cycle count per opcode is exact; peripherals advance in batches after the instruction. | Comparison of `cpu->cycles` with the official table. |
| **N2** | Ordered at bus-cycle level | Each **bus access** (actual read/write) lands on the right intra-instruction cycle; **internal** (non-bus) cycles are caught up by padding at the end of the instruction; IRQ sampled at instruction boundaries. | Observation of an I/O register with side effects (e.g. RMW double write on the VIA). |
| **N3** | Cycle-stepped | The machine's unit of progress is **the cycle**: on each cycle the CPU performs exactly one bus action (including dummy accesses) or one explicit internal cycle, and all peripherals advance by one cycle in lockstep. IRQ/NMI sampled at the penultimate cycle. | Comparison of a **cycle-by-cycle bus trace** against an external oracle (SingleStepTests/65x02). |
| **N4** | Sub-cycle (φ1/φ2 phases) | The cycle is subdivided; setup/hold races between cards and bus are modelled. | Race predicate + reproducible edge cases. |

## Current ranking, component by component

| Component | Actual level | What is missing for N3 |
|-----------|-------------|------------------------|
| **CPU 6502** (`src/cpu/`) | **N3 reached** (default since v1.124.0) | Nothing identified any more. Exact bus sequence **100.00 %** over 2,440,000 cases of the 65x02 oracle; all NMOS dummy accesses; interrupts sampled at the **penultimate cycle** (hence the delayed I flag of `CLI`/`SEI`/`PLP`); NMI hijacking during `BRK`. Deliberate divergence: the 12 **JAM** opcodes halt the CPU instead of locking the bus. The historical engine (N2) remains available through `--cpu-legacy`. |
| **VIA 6522** (`src/io/via6522.c`) | **N3 for the timers** (2.0.0-alpha.2) | Clocked per cycle (`via_update()` always receives **1**, checked by `test-clock`) with **exact underflow**: `$0000 → $FFFF` then a reload cycle, hence a compliant **N+2** period (the old model gave N — a 20 % frequency error at N=10). Checked by vectors: continuous period, one-shot time-out, one-shot reload, PB7 square wave, counter read-back, one-cycle CA2 pulse, and 50 interrupts in 50 frames. **Since 2.6.0, behaviour measured on a real 6522** (ported from Neo6502Vic20, validated there by 61/61 VICE VIC-20 test programs with hardware references: countdown starting on the cycle after the write, one-shot reload, delayed T2 mode, 8-bit T2 for the SR, PB7 without DDRB, shift register) — these programs do not run in Phosphoric's CI, where the rules are locked in by `test-io`. The **half-cycle** of the time-out (N+1.5) is not represented. **Since 2.7.0, exact lazy path** (`via_tick`, ported from Neo6502Vic20 US-31): a cycle where nothing can happen is only counted, then applied in one go before any access; the behaviour stays that of stepping every cycle, proven by `make test-via-lazy` (116 command lines, 0 differences against a `VIA_NO_LAZY=1` build) and `test_via_lazy_matches_stepwise`. Register/function conformance audited (`docs/HARDWARE_CONFORMANCE.md` §2). |
| **Video ULA** (`src/video/video.c`) | **N3 for the fetch** (2.0.0-alpha.3, intra-cycle order fixed in 2.0.2) | One **6-pixel cell fetched per cycle**, at the moment the beam reads it: a CPU write in mid-line now only affects the cells not yet scanned (**raster splits**). Ink, paper and text attributes are a line state that persists across cells. **Reference and order taken from the hardware** (Mike Brown, *Unofficial ULA Guide* 1.02, oscilloscope measurements): columns 0-39 at counts 0-39 of the horizontal counter, blanking 40-63, sync 49-52; within the cycle, the 6502 accesses first, then the ULA fetches the same count → a write at cycle *c* is seen by cell *c* (up to 2.0.1: *c+1*, one cell too far right). What remains: border and blanking not rendered (224 visible lines out of 312); absolute raster/CPU phase not modelled (unobservable without the VSYNC hack); ULA-NG full-screen extended modes rendered per line. Fallback: `--ula-line`. |
| **PSG AY-3-8910** (`src/audio/ay3891x.c`) | **clocked like the hardware** (2.0.0-alpha.4) | Machine clocked at `clock/8` = 125 kHz, the chip's actual internal step: tone `clock/(16·TP)`, LFSR `clock/(16·NP)`, envelope `clock/(8·EP)` — the latter was **2× too slow**. The output is **integrated** over the steps covered by each sample: above Nyquist the signal is attenuated instead of **aliasing** (a 62.5 kHz tone used to come out at 18.4 kHz at full amplitude). Checked by **signal measurement** (frequencies ±0.1 %, envelope ±2 %, balanced LFSR), not by comparison against frozen bytes. Retained achievement: register writes **timestamped in CPU cycles** → digidrums. **Output stage** taken from the official schematic (`docs/architecture/oric-audio-output.md`): the parallel mixing **averages** the channels (our sum/3 is therefore correct — the former "deviation" was wrong), the circuit's only low-pass filter cuts at **37 kHz** (out of band), and the capacitive coupling **blocks DC** — now modelled (DC +8188 → +15, symmetric signal). Outside the model and documented: the exact cutoff of the `C4` coupling (value unreadable on the schematic: 2.2 nF ⇒ 4.7 kHz or 2.2 µF ⇒ 4.7 Hz, a factor of one thousand), the response of the LM386 and of the internal loudspeaker. |
| **FDC WD1793** (`src/storage/disk.c`) | **N1+** (clocked per cycle; real rotational latency by default, `LOST DATA` and write-protect modelled since 2.0.0-alpha.6 — see `docs/HARDWARE_CONFORMANCE.md` §1) | **Flat-rate** DRQ/INTRQ delays (e.g. 60 cycles) instead of being derived from the rotational position; flat image model, so LOST DATA / CRC are structurally impossible (`docs/HARDWARE_CONFORMANCE.md` §1). The "accurate to the cycle" label used in the LOCI reports (CR) is **misleading** — it refers to being clocked in cycles, not to being exact to the cycle. |
| **Cassette** | **N3 in signal mode** (clocked per cycle by the master clock, frame parity fixed in 2.0.0-alpha.7) | The default path remains the **ROM patch** (fast-load), outside the timing model — a deliberate choice (US6.1): same content loaded, 2.4× fewer cycles. |
| **Expansion bus (LOCI/MIA)** | **Partial N4** | Grid of 30 φ2 sub-ticks, race predicate, seeded jitter (Epic B phases 1-2) — the only truly sub-cycle place in the project, but the constants are not calibrated on real hardware (phases 3-4 open). |

## How to measure (V2-S1)

The instruments have existed since v1.122.0-alpha; the vectors, large and
third-party, are not versioned:

```bash
tools/fetch_vectors.sh dormann     # ~800 KB
tools/fetch_vectors.sh 65x02       # ~1 GB, only once
make test-cycle                    # cycle-by-cycle oracle (200 cases/opcode)
make test-cycle CYCLE_MAX_CASES=0  # all 10,000 cases per opcode
make test-dormann                  # Klaus Dormann's functional test
```

Without vectors, both targets report **SKIP** (so they stay in `make tests` and
in CI). `make test-cycle` measures four distinct properties, from weakest to
strongest:

| Property | What it proves | Level |
|-----------|-------------------|--------|
| final state (registers + RAM) | the computation is correct | timing-independent |
| total cycles per instruction | the counters are exact | **N1** |
| bus subsequence | no spurious access, no order inversion | **N2** |
| exact bus sequence | one access at the right cycle, for **every** cycle | **N3** |

### V2-S1 reference score (2026-09-10, v1.122.0-alpha)

**Exhaustive** run: 244 opcodes (excluding the 12 JAM) × 10,000 cases =
**2,440,000 cases**, in 6.4 s.

| Property | Score | Reading |
|-----------|-------|---------|
| final state (registers) | **100.00 %** | 2,440,000 / 2,440,000 |
| final state (RAM) | **100.00 %** | 2,440,000 / 2,440,000 |
| total cycles | **100.00 %** | level N1 is proven |
| bus subsequence | **100.00 %** | **level N2 is proven** |
| exact bus sequence | **44.26 %** | the gap still to be closed for N3 |

244/244 non-JAM opcodes are at 100 % on state + RAM + cycles. The 44.26 % figure
is the **quantified starting point of V2**: it measures exactly what is
missing — the dummy accesses and the explicit internal cycles. The test's
`BUS_EXACT_FLOOR_BP` floor forbids any regression below this rate; V2-E1 must
raise it. The rate is stable to ±0.1 % from 100 cases per opcode onwards, so the
default sampled run (200) is enough for monitoring.

The oracle also revealed **5 real defects** in the core, fixed in the same
version (access order of `JSR`, `ADC`/`SBC` flags in decimal mode, decimal
`ARR`, unstable `SHA`/`SHX`/`SHY`/`SHS` stores on page crossing) — see the
CHANGELOG. Klaus Dormann's functional test passes in full
(`make test-dormann`), which confirms that the core's shortcoming is **temporal,
not logical**.

### Two cores, one computation (V2-S2/S3)

The CPU has **two engines** that share the same semantics (same computation
functions: flags, BCD, illegal opcodes) and differ only in the scheduling of
cycles. Since v1.124.0-alpha, the **micro-sequenced engine is the default**;
the historical one remains available through `--cpu-legacy`.

| | historical (`--cpu-legacy`) | micro-sequenced (**default**) |
|---|---|---|
| exact bus sequence (N3) | 44.26 % | **100.00 %** |
| cycles without address | 12.8 % of cycles at boot | **0** |
| NMOS dummy accesses | absent | all emitted |
| interrupt acceptance | instruction boundary | **penultimate cycle** |
| delayed I flag (`CLI`/`SEI`/`PLP`) | no | **yes** |
| NMI hijacking during `BRK` | no | **yes** |
| cost (frame, 20 ms budget) | 1.9 % | 2.5 % |

Evidence of identical integration between the two engines: byte-identical
**ORIC-1** and **Atmos** boots, **13 real programs** (6 Sedoric disks including
Citadelle, OricChess, L'Aigle d'Or, HHGG + 7 tapes including Manic Miner,
Atlantis, Acheron) identical on screen, Dormann test passed at the **same number
of cycles** (96,241,367).

**Observable consequences of the restored fidelity**: an interrupt can no longer
be taken *before* the current instruction (the hardware cannot do that); a line
that becomes active during the **last** cycle of an instruction is seen too late
and is only honoured after the next instruction; `SEI` does not protect the
instruction that follows it from an already pending IRQ, and `CLI`/`PLP`
symmetrically delay its arrival by one instruction.

### Dummy accesses reach side-effect registers

This is the most surprising consequence, and it is **intended**: a dummy cycle
is a real bus cycle, and a chip does not know that it is a dummy.

Case encountered in 2.0.0-alpha.3: `POKE 1021,C` in BASIC writes to the data
register of an ACIA. The ROM's POKE routine uses `STA (zp),Y`, for which the
NMOS 6502 performs a **dummy read of the address before writing** — and reading
the data register of an ACIA **consumes the received byte**. An echo program
written in BASIC therefore loses bytes, on the emulator **just as on a real
machine**. The trace shows it in two lines:

```
R $03FD $52    <- the POKE's dummy read: the byte "R" is swallowed
W $03FD $4F    <- the intended write
```

A serial driver must therefore be written in assembler, with **absolute**
`STA`/`STY`, which have no dummy cycle. The same precautions apply to any
register with destructive read (ACIA 6551/6850, FIFO registers). Before V2,
Phosphoric did not emit these accesses and let such programs work: it was more
permissive than the hardware.

The `--cycle-trace FICHIER` trace (one line per cycle: access type, address,
data, registers, interrupt lines) is used to diff a discrepancy line by line
against another emulator or instrumented hardware. Lines marked `i`
(padded internal cycles, without address) now only appear with
`--cpu-legacy`: the default core emits a real access on every cycle.

## Permitted wording

Status at the end of V2-S9 (2.0.0-alpha.8):

- ✅ **"CPU core exact to the cycle" / "cycle-stepped CPU core"** — achieved since
  v1.124.0-alpha: 100 % exact bus sequence over 2,440,000 oracle cases,
  interrupts at the penultimate cycle.
- ✅ **"machine clocked per cycle" / "cycle-stepped machine"** — achieved since
  2.0.0-alpha.8: the master clock advances **all** components one cycle at a
  time, **never idle** (the phantom cycle of the untaken branch, which made the
  ULA drift by ~410 cycles per frame, is fixed and locked down by `test-clock`),
  and a savestate is an exact resume point (`test-savestate-determinism`). VIA
  timers, ULA fetch and PSG are exact **at the level documented in the table
  above**.
- ✅ "bus-cycle-accurate" for the **whole machine**.
- ✅ "exact cycle counters per opcode (256/256)".
- ❌ "accurate to the cycle" **on its own** (without the "bus-" qualifier) or
  "exact to the cycle" for the **whole machine**: the FDC remains N1+ (flat-rate
  DRQ/INTRQ delays), the half-cycle of the VIA one-shot is not modelled, and
  neither is the absolute raster/CPU phase (VSYNC hack).
- ❌ "WD1793 accurate to the cycle" → say "WD1793 clocked in cycles, flat image model".

Each wording is backed by a test that would disprove it: `make test-cycle`
(65x02 oracle + Dormann), `make test-clock` (one call = one cycle, never idle),
`make test-io` (VIA vectors), `make test-raster-split` (ULA fetch per cycle),
`make test-savestate-determinism`, `make test-corpus` (screen fingerprints of the
local corpus). See [the V2 plan](specs/V2_CYCLE_ACCURACY.md).
