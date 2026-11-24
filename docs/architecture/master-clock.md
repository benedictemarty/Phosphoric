# Master clock — the `emu_cycle()` contract

**Module**: `src/emu_clock.c` · **Declaration**: `include/emulator.h`
**Delivered in**: V2-S4 (2.0.0-alpha.1) · **Plan**: [V2, epic E2](../specs/V2_CYCLE_ACCURACY.md)

## Why

Before this module, the machine's time was spread across three places:

1. the CPU advanced its own clock (`cpu_tick`);
2. a per-cycle callback defined **in `main.c`** (`cpu_cycle_tick`) advanced
   the φ2 peripherals;
3. **the main loop computed the beam position itself**
   (`frame_cycles / 64`) to decide when to emit a scanline.

Consequence: only the main loop knew how to clock the machine. The
debugger, the tests, the replay and the tools could not ask "advance
by one cycle" — they could only execute one instruction and hope that the
rest would follow.

## The contract

```c
bool emu_cycle(emulator_t* emu);   /* one cycle of the WHOLE machine */
int  emu_step (emulator_t* emu);   /* one instruction, via emu_cycle */
```

`emu_cycle()` returns `true` when the executed cycle completed an instruction.
The intra-cycle order is **fixed**:

| Order | Who | What |
|-------|-----|------|
| 1 | CPU | the cycle's single bus access (read, write, or NMOS dummy access) — on the hardware, this is the first event after the rising edge of the 1 MHz clock |
| 2 | φ2 peripherals | VIA, FDC, ACIA, DTL, Mageco, cassette — advanced by exactly **one** cycle by the CPU clock callback, right after the bus access |
| 3 | ULA | fetch of **one 6-pixel cell** (the column = the horizontal count of this cycle), then beam advance, ULA-NG raster tick |

### Why the CPU before the ULA — it is measured, not chosen

The ORIC's real ULA and the 6502 share the DRAM in two halves of the same
1 µs cycle, without ever contending for it (no cycle stealing to model,
unlike on a ZX Spectrum). **The order of the two halves was measured with an
oscilloscope** by Mike Brown (*ORIC 1/ATMOS Unofficial ULA Guide* 1.02,
§ Control and Sequencing): on the rising edge of the 1 MHz clock, the horizontal
counter increments and the 6502 performs its access ("*it is asserting CAS here
that performs the write*"); **then** "*the ULA access cycle begins*" —
the ULA fetches the screen byte for that same count during the low half, which is longer
(hence the asymmetric 1 MHz clock). Observable consequence: a CPU write
at cycle *c* is seen by cell *c*. Verified by
`test_cpu_write_visible_in_the_same_cell` (`make test-clock`), which fails with
the reverse order.

Up to 2.0.1, Phosphoric did the opposite (ULA first, write visible at
*c+1*): every raster split landed **one cell (6 pixels) too far right**.

The same guide fixes the **horizontal reference**: the ULA counter counts 0-63,
columns 0-39 are fetched at counts 0-39, blanking occupies 40-63 and
the sync pulse counts 49-52. "Column 0 at cycle 0 of the line"
is therefore not an emulator convention but the chip's counter;
`--ula-fetch-offset` remains an experimentation tool, its correct value is 0.
As for the **absolute phase** (which CPU cycle since reset lands on count 0
of line 0): the ULA counters run freely and the 6502 reset
is asynchronous, so no software can observe it on an unmodified ORIC —
only the "VSYNC hack" (ULA → VIA wire), not emulated, would make it visible.

### No batches

With the micro-sequenced core (the default since v1.124.0), every cycle is a
bus access: the clock callback therefore always receives `cycles = 1`, and the
φ2 peripherals are advanced **one cycle at a time**. This is verified by
`test_peripherals_get_one_cycle_at_a_time`, which would fail if a batch
reappeared.

### Never an idle call

The contract has a converse: **each call to `emu_cycle()` costs the CPU exactly
one cycle**. A micro-op that returned without a bus access would advance
the ULA by a cycle that neither the CPU nor the VIA had lived through — the CPU counter
would stay correct, the 65x02 oracle would see nothing, and yet the image would drift.
It happened (2.0.0-alpha.8): the "branch not taken" decision was taken
one cycle too late, idle, i.e. the beam ran ~410 cycles ahead per frame on
the BASIC ROM. Since then, `test_branch_not_taken_costs_no_phantom_cycle` and
`test_raster_and_cpu_stay_in_step_over_a_frame` (CPU counter **=** raster
position over a whole frame) lock it down.

### Sub-cycle (subdivided φ2)

The φ2 phase is itself subdivided into **30 sub-ticks** for the expansion
bus (epic B, `include/io/bus_timing.h`): this is where the race
between the card and the next PHI2 edge is played out. See
[phi2-bus-timing.md](phi2-bus-timing.md). The motherboard peripherals
do not need it (they always win the race), so the cost is zero for
them.

## State carried by the emulator

These counters used to be local variables of the main loop; moving them
into `emulator_t` is precisely what allows any caller to
clock the machine:

| Field | Role |
|-------|------|
| `raster_cycle` | current cycle within the frame (0 … 19967) |
| `raster_rendered` | visible scanlines already emitted (0 … 224) |
| `raster_ng_line` | ULA-NG line already processed (0 … 311) |
| `raster_next_line` | cycle of the next line crossing (fast exit) |
| `frame_cycles` | copy of `raster_cycle`, exposed to raster breakpoints |

`emu_raster_pos(emu, &line, &dot)` gives the beam position: PAL line
(0-311) and cycle within the line (0-63). It is the basis of the byte-per-cycle
fetch of epic E4.

## Frame framing

```c
emu_clock_frame_begin(emu);   /* resets the beam to zero */
while (...) emu_step(emu);    /* one frame's worth of cycles */
emu_clock_frame_end(emu);     /* finishes the remaining lines */
```

`emu_clock_frame_end()` exists for the case where the CPU stops in mid-screen
(halt, breakpoint): the displayed image must remain complete.

The main loop does not count its own cycles: it reads `raster_cycle`.
This is what allows **resuming a savestate mid-frame** (V2-E7): the
`CLK` section of the `.ost` restores `raster_cycle` / `raster_rendered` /
`raster_ng_line` and arms `clock_resume_pending`; `emu_clock_resume()` — called
by `emu_clock_frame_begin()` or by the loop if loading took place during
the frame — realigns `raster_next_line`, rebuilds from RAM the lines
already scanned (the framebuffer is not saved) and, in per-cycle ULA mode, replays
the already fetched cells of the current line to recover the line's serial
state. A state saved after the end of a frame (`-c`, `--save-state`) simply
restarts from zero. Verified by `make test-savestate-determinism`: same raster
stops, same VIA, same RAM as an uninterrupted run.

### The main loop, step by step

Since 2.0.3, `emulator_run()` (src/main.c) merely unrolls, for
each frame, **named steps** — `run_frame_instructions` (the per-instruction
loop around `emu_step`), then the end-of-frame hooks (LOCI, headless
sound, fast-load, automatic typing, SDL presentation and events,
captures, recording, pokes, pacing) — sharing a `run_state_t`
(executed cycles, frames, clocks). The order of the steps is **observable** (a
`--screenshot-at` capture sees the screen after the automatic typing of the same
frame, never before): it is that of the historical loop and must not be
reordered without a measured reason.

## Historical core

`--cpu-legacy` executes an instruction in one block and cannot stop between
two cycles. `emu_cycle()` then executes the whole instruction and catches up the
beam by the same number of cycles, and always returns `true`. The result is
identical to the old loop — verified by
`test_legacy_core_advances_by_instruction` and by the equality of screen captures
over the corpus.

## Performance

The clock is called a million times per emulated second. Two precautions:

- the division by 64 was replaced by a **fast exit** on
  `raster_next_line`: 63 cycles out of 64 only do two additions and one test;
- per-cycle fetch (epic E4) cost only **+4 %**: the total work is the
  same (40 cells per line), only its distribution changes.

Measured: **601 µs per emulated frame** on the reference machine, i.e. **3.0 %**
of the 20 ms budget — the ceiling set by the plan is 5 %.
