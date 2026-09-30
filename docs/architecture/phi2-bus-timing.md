# Sub-cycle time base for the expansion bus (PHI2 model) — Epic B

- **Status**: architecture (v1.0) — Phase 1 delivered
- **Author**: bmarty
- **Modules**: `include/io/bus_timing.h`, `src/io/loci_boot.c` (LOCI client),
  `src/io/io_bus.c` (ACIA `$0380` decision point)
- **Grounding**: `~/loci/extensions/analyse/read-serve-et-inhibition-via.md`,
  `robustesse-lien-6502.md` (firmware `sodiumlb/loci-firmware`, LOCI 1.3 schematic).

## 1. Problem

The Oric's 6502 and the **expansion port** peripherals share an
**asynchronous** bus clocked by PHI2. Phosphoric models time at the granularity of the
**whole cycle**: `cpu_tick()` advances the clock on every bus access (`cpu->cycles`
is exact at every read), but there is **no notion of intra-PHI2 phase**.

Yet some conflicts are **sub-cycle**: the data must be **stable on the bus
before the 6502's latch instant** (close to the falling edge of PHI2, after the
setup time). A **slow** peripheral — typically the LOCI, whose RP2040 samples
the bus through PIO at `sys_clk = PHI2×30` (`cpu.c:158`) and then drives the data — can
**miss** that latch. Since the VIA is decode-inhibited (see `io-bus.md`), **nothing drives
the bus** at that point → the 6502 latches open-bus. At whole-cycle scale, this
phenomenon is **invisible**: the 6502 read and the serve fall "in the same
cycle". A **sub-cycle** time base is therefore needed to reproduce it.

## 2. Model (Phase 1)

Grid: the PHI2 period is divided into `BUS_PHI2_SUBTICKS = 30` (the LOCI's
sys_clk/PHI2 ratio, independent of the actual PHI2 frequency → everything is in
**fractions of a period**).

- The 6502 **latches** the data at subtick `latch_subtick` (default 27 = end of
  PHI2 high minus the setup time).
- A peripheral makes its data valid at subtick `valid_subtick`.
- **Clean** read iff `valid_subtick ≤ latch_subtick`; otherwise the **race is lost**
  (open-bus). Predicate: `bus_serve_wins_race()` (`bus_timing.h`).

**On-board** peripherals (RAM/ROM/VIA/ULA) are valid early
(`valid_subtick = 0`) → they always win → **no impact**. Only expansion-port
peripherals with a slow serve can lose. This is the "global" implementation,
but **at zero cost for existing code**: the layer is general, but on-board
accesses are not routed through it (they would always win).

### LOCI client (`loci_mia_io_reliable`)

Two mutually exclusive reliability models for the MIA serve:

- **WINDOW** (default, historical): reliable iff `tior ∈ [lo,hi]`. This is the
  **per-board calibration** (the firmware's `adj_scan` sweeps tior 0-31 to find
  the working range). Behaviour unchanged; `--loci-mia-window LO-HI`.
- **PHASE** (opt-in, physically grounded): the serve arrives at subtick
  `tior + serve_subticks`; clean iff `≤ latch_subtick`. `--loci-serve-timing
  SERVE[,LATCH]`. Makes explicit two factors that WINDOW hides:
  - the **serve budget** (≈ the firmware build): the analysis measures ~26 M0+ cycles with
    `-Os` (optimised) vs ~36 at baseline. At `latch=27`: `serve=26 → clean`,
    `serve=36 → missed`. **Reproduces the bug report exactly** (the `-Os` rebuild
    fixes the `$0380` read).
  - **independence from the PHI2 frequency** (grid in fractions of a period).

`loci_set_mia_window()` switches to WINDOW, `loci_set_serve_timing()` to PHASE.
Default at reset: WINDOW `[0,31]` → every tior is reliable.

## 3. What Phase 1 does not do (yet)

- **No sub-cycle rewrite of the 6502.** `cpu_step` executes a whole
  instruction; the phase model lives at the **bus access decision point** (memory
  read → io peripheral), where the race matters. A deep sub-cycle integration
  of the CPU (each access = one bus cycle timestamped with its phase) is a later
  phase.
- **No jitter.** The decision is deterministic (reproducible tests). Seeded
  jitter within the marginal band is a future option.
- **A single client** (LOCI). Other expansion-port peripherals
  would plug into the same predicate through their own `valid_subtick`.

## 4. Roadmap (epic B)

- [x] **Phase 1** — `bus_timing.h` foundation (PHI2×30 grid, latch, race
      predicate) + LOCI client (opt-in PHASE model, CLI `--loci-serve-timing`) +
      tests. Behaviour unchanged by default.
- [ ] **Phase 2** — plug the other expansion-port peripherals into the
      predicate (each with its own valid_subtick); optional seeded jitter.
- [ ] **Phase 3** — sub-cycle timestamping of accesses at CPU level (each access
      carries its phase); on-board setup/hold if a real case requires it.
- [ ] **Phase 4** — calibrate the constants (latch, serve budgets) against
      real hardware (the current values are modelled within the ranges of
      the analysis, not measured to the picosecond).

## 5. References

- `include/io/bus_timing.h` — grid, predicate.
- `src/io/loci_boot.c` — `loci_mia_io_reliable`, `loci_set_serve_timing`.
- `src/io/io_bus.c` — application to the ACIA `$0380` (open-bus + destructive read).
- `~/loci/extensions/analyse/read-serve-et-inhibition-via.md` — serve 26-36 cycles,
  sys_clk = PHI2×30, `-Os` vs `-O2`.
- `docs/architecture/io-bus.md` — page 3 dispatch, VIA inhibition.
