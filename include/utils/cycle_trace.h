/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cycle_trace.h
 * @brief Cycle-by-cycle bus trace (--cycle-trace) -- a V2 instrument
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-10
 *
 * One line per CPU cycle, with the cycle, the kind of access, the address
 * driven, the byte on the data bus and the register state. Meant to be
 * diffed against another emulator or against instrumented hardware, and to
 * serve as evidence in the V2 sprints (docs/specs/V2_CYCLE_ACCURACY.md).
 *
 * Format (fixed columns, space separator):
 *   CYCLE      T ADDR  DATA PC    A  X  Y  SP P  FLAGS  IRQ
 *   0000000000 R $FFFC $A5  $0000 00 00 00 FD 34 ..-..I. .
 *
 *   T    = R bus read, W bus write, i internal cycle
 *   IRQ  = '.' no active line, otherwise the source mask in hex
 *
 * ⚠️ At the current accuracy level (**N2**, see docs/ACCURACY.md), the cycles
 * marked `i` are the internal cycles **padded at the end of the instruction**:
 * they are counted at the right place in the total, but a real 6502 drives an
 * address on them (often a dummy access) that Phosphoric does not have yet. Once
 * V2-E1 has delivered the micro-sequenced core, these lines will carry their real
 * access and the trace will become usable exact to the cycle.
 */

#ifndef CYCLE_TRACE_H
#define CYCLE_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu6502.h"

/**
 * @brief Opens the trace file and arms the capture
 *
 * @param path      File path (overwritten)
 * @param max_lines Line cap (0 = unlimited)
 * @return true if the file is open
 */
bool cycle_trace_open(const char* path, uint64_t max_lines);

/** @brief true if the capture is armed (and has not yet reached the cap) */
bool cycle_trace_active(void);

/**
 * @brief Bus access callback to pass to cpu_set_bus_callback()
 *
 * Records the access; the line is written at the matching cycle by
 * cycle_trace_cycles(), so that cycle numbering stays exact.
 */
void cycle_trace_bus(void* ctx, uint16_t addr, uint8_t value, bool write);

/**
 * @brief Advances the trace by `cycles` cycles (to be called from the clock hook)
 *
 * Writes the line of the recorded bus access (if any) then one `i` line
 * per remaining internal cycle.
 */
void cycle_trace_cycles(const cpu6502_t* cpu, int cycles);

/** @brief Closes the file (idempotent) and returns the number of lines written */
uint64_t cycle_trace_close(void);

#endif /* CYCLE_TRACE_H */
