/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file emu_clock.c
 * @brief Master clock: one call = one cycle of the WHOLE machine (V2-E2)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * Before this module, machine time was split across three places:
 * the CPU advanced its clock, a per-cycle callback (`cpu_cycle_tick` in
 * main.c) advanced the φ2 devices, and **the main loop computed the
 * beam position itself** (`frame_cycles / 64`) to
 * emit the scanlines. Consequence: only the main loop knew how to
 * clock the machine; the debugger, the tests and the tools could not
 * advance « one machine cycle ».
 *
 * `emu_cycle()` is that single point, with a **fixed intra-cycle order**:
 *
 *   1. **the CPU** executes its single cycle: one bus access (read, write,
 *      or NMOS dummy access) with the micro-sequenced core. On the hardware,
 *      this is the first thing that happens after the rising edge of the
 *      1 MHz clock: the ULA's horizontal counter increments and the 6502 makes its
 *      DRAM access (its CAS is what writes).
 *   2. **the φ2 devices** (VIA, FDC, ACIA, DTL, Mageco, cassette) are
 *      advanced by exactly one cycle by the CPU clock callback, right after
 *      the bus access. With the default core this callback always receives
 *      `cycles = 1`: the devices are therefore already at cycle level, with no batching
 *      (checked by `test-clock`).
 *   3. **the ULA** THEN fetches the screen byte of that same count (low half of the
 *      cycle — the ULA takes more than half, hence the asymmetric 1 MHz clock),
 *      then the beam advances and the due scanlines / the ULA-NG tick are emitted.
 *      A CPU write at cycle *c* is therefore seen by cell *c*.
 *
 * This order is the one MEASURED on the oscilloscope by Mike Brown (Unofficial ULA
 * Guide 1.02, « Control and Sequencing »): 6502 first, then « the ULA access
 * cycle begins ». Up to 2.0.1 Phosphoric did the opposite (ULA then CPU,
 * write visible at c+1): a raster split landed one cell too far right.
 * On the ORIC, the ULA and the CPU do not contend for RAM (accesses in opposite
 * phases): there is therefore no cycle stealing to model, unlike a
 * ZX Spectrum.
 *
 * The historical core (`--cpu-legacy`) cannot stop between two cycles:
 * `emu_cycle()` then executes a whole instruction and catches the
 * beam up by the same number of cycles. The result is identical to the old
 * loop, except for the internal ordering.
 */

#include "emulator.h"
#include "cpu/microseq.h"
#include "video/video.h"
#include "io/ula_ng.h"

/* ─── Cycle-level ULA (V2-E4) ───
 * Emits the video work of the current cycle: line start, fetch of one cell,
 * line end. It happens AFTER the CPU access of the same cycle (order measured on
 * the hardware), so a CPU write during this cycle is seen by the cell
 * fetched in this cycle — and a mid-line write only affects the
 * cells not yet fetched.
 *
 * `dot` is the cycle within the line (0-63). The fetched column is
 * `dot - ula_fetch_offset`: 40 visible cells, the rest of the line being
 * border and blanking. */
/* Per-cycle fetch requires a core able to stop between two cycles: with
 * `--cpu-legacy`, the instruction is indivisible, so we fall back to per-line
 * rendering. Without this guard, the screen would stay black in this mode. */
static bool ula_cycle_in_use(const emulator_t* emu) {
    return emu->ula_per_cycle && cpu_microseq_enabled(&emu->cpu);
}

static void ula_cycle(emulator_t* emu, int line, int dot) {
    if (line >= 224) return;                    /* vertical blanking */
    const uint8_t* mem = emu->memory.ram;

    if (dot == 0) video_line_begin(&emu->video, mem, line);

    int col = dot - emu->ula_fetch_offset;
    if (col >= 0 && col <= 40)
        video_render_cell(&emu->video, mem, line, col);

    if (dot == PAL_CYCLES_PER_LINE - 1) {
        video_line_end(&emu->video, mem, line);
        emu->raster_rendered = line + 1;        /* this line is complete */
    }
}

/* Advances the beam by `cycles` cycles: emits the due visible scanlines
 * (active area 0-223) and the ULA-NG raster ticks (full frame 0-311). */
static void clock_advance_raster(emulator_t* emu, int cycles) {
    emu->raster_cycle += cycles;
    emu->frame_cycles = emu->raster_cycle;   /* exposed to raster breakpoints */

    /* Fast exit: 63 cycles out of 64 cross no end of line and
     * therefore have nothing to emit. This test replaces a per-cycle division —
     * the clock being called a million times per emulated second, it matters. */
    if (emu->raster_cycle < emu->raster_next_line) return;

    do {
        /* Scanline rendering: the whole line samples memory at the instant
         * the beam finishes it. In cycle-level ULA mode, rendering has already been
         * done cell by cell by ula_cycle() — nothing to do here. */
        if (!ula_cycle_in_use(emu) && emu->raster_rendered < 224) {
            video_render_scanline(&emu->video, emu->memory.ram, emu->raster_rendered);
            emu->raster_rendered++;
        }
        /* ULA-NG raster over the full PAL frame, decoupled from the visible area:
         * asserts the IRQ line when the programmed line is crossed. */
        if (emu->raster_ng_line < ULA_NG_FRAME_LINES) {
            ula_ng_scanline(&emu->ula_ng, emu->raster_ng_line);
            if (ula_ng_irq(&emu->ula_ng)) cpu_irq_set(&emu->cpu, IRQF_ULANG);
            emu->raster_ng_line++;
        }
        emu->raster_next_line += PAL_CYCLES_PER_LINE;
    } while (emu->raster_cycle >= emu->raster_next_line);
}

bool emu_cycle(emulator_t* emu) {
    if (cpu_microseq_enabled(&emu->cpu)) {
        /* Order MEASURED on the hardware (Mike Brown, Unofficial ULA Guide 1.02):
         * on the rising edge of the 1 MHz clock, the horizontal counter increments
         * and the 6502 makes its access (its CAS is what writes); the ULA THEN
         * fetches the byte of that same count, during the low half of the cycle.
         * A CPU write at cycle c is therefore seen by cell c. */
        bool last = cpu_cycle(&emu->cpu);    /* the CPU, then its φ2 devices */
        if (ula_cycle_in_use(emu))
            ula_cycle(emu, emu->raster_cycle / PAL_CYCLES_PER_LINE,
                      emu->raster_cycle % PAL_CYCLES_PER_LINE);
        clock_advance_raster(emu, 1);
        return last;
    }
    /* Historical core: indivisible. One instruction, then the beam. */
    int n = cpu_step(&emu->cpu);
    clock_advance_raster(emu, n);
    return true;
}

int emu_step(emulator_t* emu) {
    uint64_t before = emu->cpu.cycles;
    while (!emu_cycle(emu)) {
        if (emu->cpu.halted) break;
    }
    return (int)(emu->cpu.cycles - before);
}

void emu_clock_resume(emulator_t* emu) {
    if (!emu->clock_resume_pending) return;
    emu->clock_resume_pending = false;

    /* State saved after an end of frame (exit on `-c`, `--save-state`): the
     * frame was over, the next one starts at zero. */
    if (emu->raster_cycle >= CYCLES_PER_FRAME || emu->raster_cycle < 0) {
        emu->raster_cycle = 0;
        emu->raster_rendered = 0;
        emu->raster_ng_line = 0;
        emu->raster_next_line = PAL_CYCLES_PER_LINE;
        emu->frame_cycles = 0;
        return;
    }

    int line = emu->raster_cycle / PAL_CYCLES_PER_LINE;
    int dot  = emu->raster_cycle % PAL_CYCLES_PER_LINE;
    emu->raster_next_line = (line + 1) * PAL_CYCLES_PER_LINE;
    emu->frame_cycles = emu->raster_cycle;

    /* Lines already scanned before the save: rendered in one block from the restored
     * RAM (same approximation as the end of frame after a halt). */
    int done = line < 224 ? line : 224;
    for (int y = 0; y < done; y++)
        video_render_scanline(&emu->video, emu->memory.ram, y);
    emu->raster_rendered = done;

    /* Current line in cycle-level ULA mode: replays the already-fetched cells
     * to rebuild the serial line state (ink, paper, attributes). */
    if (ula_cycle_in_use(emu) && line < 224) {
        video_line_begin(&emu->video, emu->memory.ram, line);
        int col_end = dot - emu->ula_fetch_offset;
        for (int col = 0; col < col_end && col <= 40; col++)
            video_render_cell(&emu->video, emu->memory.ram, line, col);
    }
}

void emu_clock_frame_begin(emulator_t* emu) {
    if (emu->clock_resume_pending) {
        emu_clock_resume(emu);
        return;
    }
    emu->raster_cycle = 0;
    emu->raster_rendered = 0;
    emu->raster_ng_line = 0;
    emu->raster_next_line = PAL_CYCLES_PER_LINE;
    emu->frame_cycles = 0;
}

void emu_clock_frame_end(emulator_t* emu) {
    /* Finishes the frame even if the CPU stopped mid-screen (halt,
     * breakpoint): the displayed image must be complete. The remaining lines are
     * then rendered in one block — they were not scanned, there is no
     * intermediate position to honour. */
    while (emu->raster_rendered < 224) {
        video_render_scanline(&emu->video, emu->memory.ram, emu->raster_rendered);
        emu->raster_rendered++;
    }
}

void emu_raster_pos(const emulator_t* emu, int* line, int* dot) {
    if (line) *line = emu->raster_cycle / PAL_CYCLES_PER_LINE;
    if (dot)  *dot  = emu->raster_cycle % PAL_CYCLES_PER_LINE;
}
