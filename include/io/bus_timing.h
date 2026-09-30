/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file bus_timing.h
 * @brief Sub-cycle time base of the Oric expansion bus (PHI2 model). Epic B, Phase 1.
 *
 * The Oric's 6502 and the expansion-port devices share an **asynchronous** bus
 * clocked by PHI2. At whole-cycle granularity (what `cpu_step`/`cpu_tick`
 * model), every access « succeeds »: the 6502 read and the device's
 * response fall in the same cycle. But some conflicts are
 * **sub-cycle** phenomena: the data must be **stable on the bus before the
 * latch instant** of the 6502 (close to the falling edge of PHI2, after the setup
 * time). A **slow** device (typically the LOCI: the RP2040 samples
 * the bus via PIO at `sys_clk = PHI2×30` then drives the data) can **miss** this
 * latch → the 6502 latches an undriven bus (open-bus).
 *
 * This module provides the grid and the race predicate, independently of the
 * PHI2 frequency (everything is expressed in **fractions of a period**, hence in subticks).
 *
 * Model (Phase 1):
 *   - the PHI2 period is divided into `BUS_PHI2_SUBTICKS` (= 30, because the LOCI clocks
 *     its PIO at PHI2×30, `cpu.c:158`);
 *   - the 6502 **latches** the read data at subtick `latch_subtick` (end of PHI2
 *     high minus setup);
 *   - a device makes its data **valid** at subtick `valid_subtick`;
 *   - the read is **clean** iff `valid_subtick <= latch_subtick`, otherwise the
 *     race is **lost** (open-bus).
 *
 * **On-board** devices (RAM/ROM/VIA/ULA) are, by definition, valid
 * early (`valid_subtick = 0`) → they always win the race → no impact. Only
 * slow-serving expansion-port devices (LOCI today) can
 * lose. This is the « global » implementation, but at zero cost for existing code.
 *
 * NB: the sub-cycle constants (`latch`, serve budget) are
 * **modelled/calibratable** values within the ranges established by the analysis
 * (`~/loci/extensions/analyse/read-serve-et-inhibition-via.md`: serve 26-36 cyc
 * M0+, sys_clk = PHI2×30), to be refined on real hardware — they do not claim
 * picosecond exactness.
 */
#ifndef BUS_TIMING_H
#define BUS_TIMING_H

#include <stdint.h>
#include <stdbool.h>

/** Subdivisions of one PHI2 period. 30 = the LOCI's sys_clk/PHI2 ratio (cpu.c:158). */
#define BUS_PHI2_SUBTICKS   30

/** Instant at which the 6502 latches the data, in subticks (end of cycle minus
 *  setup). Modelled default: the 6502 samples close to the falling edge. */
#define BUS_LATCH_SUBTICK_DEFAULT   27

/**
 * @brief Does the device's data arrive in time for the 6502 latch?
 * @param valid_subtick Subtick at which the data becomes stable on the bus.
 * @param latch_subtick 6502 latch subtick.
 * @return true if the serve wins the race (clean read), false if it loses it
 *         (open-bus). An on-board device passes `valid_subtick = 0` → always true.
 */
static inline bool bus_serve_wins_race(uint16_t valid_subtick, uint8_t latch_subtick) {
    return valid_subtick <= (uint16_t)latch_subtick;
}

/* ── Deterministic jitter (Phase 2) ──────────────────────────────────────────
 * On the real bus, the timing margin is not binary: clock noise,
 * temperature, tolerances → close to the latch boundary, some accesses get through
 * and others miss (the bug report notes it: « occasional », depends on the
 * build/board). This is modelled by a random offset of the serve's validity
 * subtick, drawn from a **seeded** PRNG → reproducible (deterministic tests), no
 * dependency on the wall clock. */

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
 * @brief Draws a symmetric jitter offset in [-amp, +amp] subticks.
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
