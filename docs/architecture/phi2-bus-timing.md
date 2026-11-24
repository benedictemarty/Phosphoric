# Sub-cycle time base for the expansion bus (PHI2 model) — Epic B

- **Status**: architecture (v2.0, 2.23.0) — timeline in ns; Phase 1 delivered
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
setup time). A **slow** peripheral — typically the LOCI, whose RP2040 reads the address
through PIO, processes it in software (`act_loop`) and then drives the data through PIO — can
**miss** that latch (the expansion port has no RDY). Since the VIA is decode-inhibited (see `io-bus.md`), **nothing drives
the bus** at that point → the 6502 latches open-bus. At whole-cycle scale, this
phenomenon is **invisible**: the 6502 read and the serve fall "in the same
cycle". A **sub-cycle** time base is therefore needed to reproduce it.

## 2. Model: timeline in ns (2.23.0)

> **Correction 2.23.0.** v1 divided the **Oric's** PHI2 period into 30
> "subticks", reading `sys_clk = PHI2×30` (`cpu.c:158`). But the Φ2 in `cpu.c` is
> a **firmware setting**, 4000 kHz by default (empty configuration): the PIO
> runs at 120 MHz, 1 tick = 8.33 ns, four times finer. With v1, a real
> LOCI would have missed all its reads. On top of that, the "26/36 cycle budget" from the bug report
> was taken to be subticks.

Origin: the falling edge of PHI2 that opens the cycle (`bus_timing.h`,
`bus_loci_read_valid_ps`, in picoseconds):

| Step | Instant | Source |
|---|---|---|
| action word in the FIFO | (22 + tior) ticks + 2 sys cycles | `mia.pio` (estimated) |
| data ready (DMA + IRQ 5) | + poll (0, not measured) + `serve` sys cycles | SysTick (`--loci-hw`) |
| data on the bus | max(ready, PHI2 rise + 2 cycles) + (3 + tiod) ticks | `mia_io_read` (estimated) |
| 6502 deadline | period − tDSR (100 ns) | 6502 datasheet at 1 MHz |

The Oric's PHI2 is high during the **last third** of the cycle (the 12 MHz ULA splits the cycle
into 3 slots of 4 cycles, two for video, one for the processor; Defence
Force forum t=2583). With the defaults: data at 708 ns as long as serve ≤ 58 cycles (it
waits for the PHI2 rise), deadline 900 ns, boundary at **81/82 cycles** of serve.
Hardware measurement (Feather 5723, `--loci-hw`): serve 23 cycles → margin ≈ 190 ns.

**On-board** peripherals (RAM/ROM/VIA/ULA) do not go through this model: they
are always on time, **no impact** on existing code.

### LOCI client (`loci_mia_io_reliable`)

Two mutually exclusive reliability models for the MIA serve:

- **WINDOW** (default, historical): reliable iff `tior ∈ [lo,hi]`. This is the
  **per-board calibration** (the firmware's `adj_scan` sweeps tior 0-31 to find
  the working range). Behaviour unchanged; `--loci-mia-window LO-HI`.
- **PHASE** (opt-in): the timeline above, `--loci-serve-timing SERVE[,TDSR]`
  (SERVE in core 1 cycles, TDSR in ns) and `--loci-serve-jitter AMP[,SEED]`
  (± AMP cycles, seeded). `tior` and `tiod` (`MAP_TUNE_*`) enter the computation.
  Realistic durations (23 measured, 26/36 from the analysis) are **all clean**:
  v1 "reproduced" the `-Os`/`-O2` bug report with a wrong grid, and
  the firmware author attributes that bug to an address *mapping* defect, not to
  timing (`read-serve-et-inhibition-via.md`, correction of 2026-09-01).

`loci_set_mia_window()` switches to WINDOW, `loci_set_serve_timing()` to PHASE.
Default at reset: WINDOW `[0,31]` → every tior is reliable.

## 3. What Phase 1 does not do (yet)

- **No sub-cycle rewrite of the 6502.** `cpu_step` executes a whole
  instruction; the phase model lives at the **bus access decision point** (memory
  read → io peripheral), where the race matters. A deep sub-cycle integration
  of the CPU (each access = one bus cycle timestamped with its phase) is a later
  phase.
- **A single client** (LOCI, emulated and `--loci-hw`). Other expansion-port
  peripherals would have their own timeline.

## 4. Roadmap (epic B)

- [x] **Phase 1** — `bus_timing.h` foundation + LOCI client (opt-in PHASE model, CLI
      `--loci-serve-timing`) + tests; seeded jitter. Behaviour unchanged by default.
      2.23.0: ns timeline shared with `--loci-hw`.
- [ ] **Phase 2** — other expansion-port peripherals.
- [ ] **Phase 3** — sub-cycle timestamping of accesses at CPU level (each access
      carries its phase); on-board setup/hold if a real case requires it.
- [ ] **Phase 4** — calibration against a real bus: PIO counts, act_loop poll,
      6502 tDSR at 2 MHz (serve and act are already measured by `--loci-hw`).

## 5. References

- `include/io/bus_timing.h` — timeline, deadline, jitter.
- `src/io/loci_boot.c` — `loci_mia_io_reliable`, `loci_set_serve_timing`.
- `src/io/io_bus.c` — application to the ACIA `$0380` (open-bus + destructive read).
- `~/loci/extensions/analyse/read-serve-et-inhibition-via.md` — `-Os`/`-O2`
  hypothesis (obsolete, cause = address mapping).
- `src/io/loci_hw.c` — serve/act measurements on real hardware; `docs/loci.md`.
- `docs/architecture/io-bus.md` — page 3 dispatch, VIA inhibition.
