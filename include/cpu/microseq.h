/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file microseq.h
 * @brief Micro-sequenced 6502 core: one cycle = one state (V2-E1)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * The historical engine (`cpu_execute_opcode`, opcodes.c) executes an
 * instruction in one block: bus accesses come out in the right order, but
 * internal cycles are caught up by padding at the end of the instruction and
 * the NMOS **dummy** accesses do not exist. This is level **N2** (docs/ACCURACY.md).
 *
 * This engine breaks each instruction down into **a plan of micro-operations,
 * one per cycle**: each cycle emits exactly its bus access — including the
 * dummy accesses (zero-page indexing, page crossing, RMW write-back, dead
 * stack reads) — or is an explicit internal cycle. This is level **N3**
 * targeted by V2.
 *
 * Both engines share the **same semantics**: the computation (flags, BCD,
 * illegal opcodes) comes from the same functions (`cpu_rmw_apply` & co. in
 * opcodes.c). They differ only in scheduling.
 *
 * Until the migration is complete, the engine is **opt-in**
 * (`cpu_set_microseq()`, CLI `--cpu-microseq`): the default path remains
 * the historical one, so no regression is possible. The `make test-cycle`
 * oracle measures both (`CYCLE_ENGINE=microseq`).
 */

#ifndef CPU_MICROSEQ_H
#define CPU_MICROSEQ_H

#include <stdbool.h>
#include "cpu/cpu6502.h"

/**
 * @brief Enables or disables the micro-sequenced engine
 *
 * Only call at an instruction boundary (otherwise the current instruction
 * would be abandoned). No effect on semantics, only on cycle scheduling.
 */
void cpu_set_microseq(cpu6502_t* cpu, bool enabled);

/** @brief true if the micro-sequenced engine is active */
bool cpu_microseq_enabled(const cpu6502_t* cpu);

/**
 * @brief Executes EXACTLY one CPU cycle
 *
 * On the first cycle of an instruction, first decides whether an interrupt
 * (NMI/IRQ) must be taken; otherwise reads the opcode and builds its plan.
 *
 * @return true if the executed cycle was the LAST one of the instruction (or
 *         of the interrupt sequence), false if the instruction continues.
 */
bool cpu_cycle(cpu6502_t* cpu);

#endif /* CPU_MICROSEQ_H */
