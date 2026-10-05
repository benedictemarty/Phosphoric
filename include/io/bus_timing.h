/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file bus_timing.h
 * @brief Sub-cycle timeline of a LOCI read on the Oric expansion bus.
 *
 * The Oric's 6502 and the expansion-port devices share an **asynchronous** bus
 * clocked by PHI2, without RDY: a device cannot slow the cycle down, it must
 * drive its data before the 6502 latches it on the falling edge of PHI2 (minus
 * the setup time tDSR). At whole-cycle granularity (`cpu_step`/`cpu_tick`),
 * every read « succeeds »; in reality a device that is too slow makes the 6502
 * read an undriven bus (open-bus).
 *
 * Timeline of a `$03xx` read served by the LOCI, in picoseconds from the falling
 * edge of PHI2 that opens the cycle (2.23.0):
 *   - the LOCI's PIO runs at Φ2cfg × 30, where Φ2cfg is a firmware SETTING,
 *     4000 kHz by default (cpu.c, empty configuration) → 1 tick = 8.33 ns. It is
 *     not the Oric's clock (the former Oric « PHI2×30 » grid was wrong);
 *   - `mia_action` pushes the word into the FIFO (22 + tior) ticks after the edge,
 *     plus 2 sys cycles of input synchroniser (counts read from mia.pio, estimated);
 *   - `act_loop` picks it up (poll, not measured) and, SERVE sys cycles later,
 *     triggers the read-serve DMA and IRQ 5 of `mia_io_read`;
 *   - `mia_io_read` waits for PHI2 high then drives the bus (3 + tiod) ticks later:
 *     data = max(ready, PHI2 rise + synchroniser) + (3 + tiod) ticks;
 *   - the Oric's PHI2: high during the last third of the cycle (the ULA gives 2/3
 *     low, 1/3 high; Defence Force forum t=2583);
 *   - 6502 deadline: end of the cycle minus tDSR (100 ns, 6502 datasheet at 1 MHz).
 *
 * **On-board** devices (RAM/ROM/VIA/ULA) do not go through this model: they are
 * always in time. Still estimates, to be confirmed on a real bus: the PIO counts,
 * the act_loop poll, the tDSR of the 6502 at the Oric's 2 MHz.
 * Measurement (Feather 5723, LOCI_USB firmware): serve 23 cycles → data at ≈ 708 ns.
 */
#ifndef BUS_TIMING_H
#define BUS_TIMING_H

#include <stdint.h>
#include <stdbool.h>

/** PIO counts from mia.pio (mia_action up to the push, mia_io_read after the IRQ). */
#define BUS_LOCI_PUSH_TICKS        22
#define BUS_LOCI_OUT_TICKS         3
/** Default firmware Φ2 setting (kHz) and the resulting system clock. */
#define BUS_LOCI_PHI2CFG_KHZ       4000u
#define BUS_LOCI_SYS_KHZ           120000u
/** Oric: PHI2 period 1 µs, high during the last third; 6502 tDSR. */
#define BUS_ORIC_PERIOD_PS         1000000
#define BUS_ORIC_TDSR_NS_DEFAULT   100

typedef struct {
    uint32_t sys_khz;      /* core 1 clock (serve cycles) */
    uint32_t pio_khz;      /* PIO clock = Φ2cfg × 30 */
    int64_t  period_ps;    /* the Oric's PHI2 period */
    int64_t  high_ps;      /* duration of PHI2 high */
    int64_t  tdsr_ps;      /* 6502 data setup time */
    int64_t  poll_ps;      /* FIFO → act_loop (not measured) */
} bus_loci_timing_t;

static inline bus_loci_timing_t bus_loci_timing_default(void) {
    bus_loci_timing_t t;
    t.sys_khz   = BUS_LOCI_SYS_KHZ;
    t.pio_khz   = BUS_LOCI_PHI2CFG_KHZ * 30u;
    t.period_ps = BUS_ORIC_PERIOD_PS;
    t.high_ps   = BUS_ORIC_PERIOD_PS / 3;
    t.tdsr_ps   = (int64_t)BUS_ORIC_TDSR_NS_DEFAULT * 1000;
    t.poll_ps   = 0;
    return t;
}

/** n periods of a `khz` kHz clock, in ps (no accumulated rounding). */
static inline int64_t bus_ps(int64_t n, uint32_t khz) {
    return n * 1000000000LL / (int64_t)khz;
}

/**
 * @brief Instant (ps after the falling edge of PHI2) at which the data of a `$03xx`
 *        read served in `serve` core-1 cycles is on the bus.
 */
static inline int64_t bus_loci_read_valid_ps(const bus_loci_timing_t* t, unsigned tior,
                                             unsigned tiod, int64_t serve) {
    int64_t sync  = bus_ps(2, t->sys_khz);
    int64_t ready = bus_ps(BUS_LOCI_PUSH_TICKS + (int64_t)tior, t->pio_khz) + sync
                  + t->poll_ps + bus_ps(serve, t->sys_khz);
    int64_t rise  = t->period_ps - t->high_ps + sync;
    return (ready > rise ? ready : rise) + bus_ps(BUS_LOCI_OUT_TICKS + (int64_t)tiod, t->pio_khz);
}

/** 6502 deadline: end of the cycle minus tDSR. */
static inline int64_t bus_loci_deadline_ps(const bus_loci_timing_t* t) {
    return t->period_ps - t->tdsr_ps;
}

/** Does the data arrive before the deadline? (false = open-bus) */
static inline bool bus_loci_read_in_time(const bus_loci_timing_t* t, unsigned tior,
                                         unsigned tiod, int64_t serve) {
    return bus_loci_read_valid_ps(t, tior, tiod, serve) <= bus_loci_deadline_ps(t);
}

/* ── Deterministic jitter (Phase 2) ──────────────────────────────────────────
 * On the real bus, the timing margin is not binary: clock noise,
 * temperature, tolerances → close to the latch boundary, some accesses get through
 * and others miss (the bug report notes it: « occasional », depends on the
 * build/board). This is modelled by a random offset of the serve duration
 * (in core-1 cycles), drawn from a **seeded** PRNG → reproducible (deterministic
 * tests), no dependency on the wall clock. */

/** xorshift32: minimal deterministic PRNG. `*state` must never be 0. */
static inline uint32_t bus_jitter_rand(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/**
 * @brief Draws a symmetric jitter offset in [-amp, +amp] cycles.
 * @param state PRNG (advanced on each call; seed it via bus_jitter_seed()).
 * @param amp Amplitude (0 = no jitter → returns 0 without advancing the state).
 */
static inline int bus_jitter_sample(uint32_t* state, uint8_t amp) {
    if (amp == 0) return 0;
    uint32_t span = (uint32_t)amp * 2u + 1u;         /* [-amp .. +amp] */
    return (int)(bus_jitter_rand(state) % span) - (int)amp;
}

/** Seed of the jitter PRNG (never 0: xorshift degenerates at 0). */
static inline uint32_t bus_jitter_seed(uint32_t seed) {
    return seed ? seed : 0xA5A5A5A5u;
}

#endif /* BUS_TIMING_H */
