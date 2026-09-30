/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cpu_internal.h
 * @brief Internal CPU functions shared between cpu6502.c, addressing.c, opcodes.c
 */

#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu/cpu6502.h"

uint8_t cpu_mem_read(cpu6502_t* cpu, uint16_t addr);
void cpu_mem_write(cpu6502_t* cpu, uint16_t addr, uint8_t val);

/* Advance the CPU clock by `cycles` and notify the per-cycle callback (if set). */
void cpu_tick(cpu6502_t* cpu, int cycles);
uint8_t cpu_fetch_byte(cpu6502_t* cpu);
uint16_t cpu_fetch_word_pc(cpu6502_t* cpu);

uint16_t addr_immediate(cpu6502_t* cpu);
uint16_t addr_zero_page(cpu6502_t* cpu);
uint16_t addr_zero_page_x(cpu6502_t* cpu);
uint16_t addr_zero_page_y(cpu6502_t* cpu);
uint16_t addr_absolute(cpu6502_t* cpu);
uint16_t addr_absolute_x(cpu6502_t* cpu, bool* page_crossed);
uint16_t addr_absolute_y(cpu6502_t* cpu, bool* page_crossed);
uint16_t addr_indirect(cpu6502_t* cpu);
uint16_t addr_indexed_indirect(cpu6502_t* cpu);
uint16_t addr_indirect_indexed(cpu6502_t* cpu, bool* page_crossed);
uint16_t addr_relative(cpu6502_t* cpu);

void cpu_push(cpu6502_t* cpu, uint8_t val);
uint8_t cpu_pull(cpu6502_t* cpu);
void cpu_push_word(cpu6502_t* cpu, uint16_t val);
uint16_t cpu_pull_word(cpu6502_t* cpu);

/* ─── ALU semantics shared between the historical engine (opcodes.c) and the
 * cycle-by-cycle micro-sequencer (microseq.c, V2-E1).
 *
 * A single implementation of each operation: the two engines differ in the
 * ORDERING of bus accesses, never in the computation. This is what guarantees
 * that a semantics fix (e.g. the BCD flags) benefits both. */

/** Read-modify-write operations (official and illegal) */
typedef enum {
    RMW_ASL, RMW_LSR, RMW_ROL, RMW_ROR, RMW_INC, RMW_DEC,
    RMW_SLO, RMW_RLA, RMW_SRE, RMW_RRA, RMW_DCP, RMW_ISC
} cpu_rmw_t;

/**
 * @brief Applies an RMW operation to an already-read value
 *
 * Updates the flags (and the accumulator for the illegal combined
 * forms) and returns the value to write back. Performs NO bus access:
 * the caller places the read and write cycles itself.
 */
uint8_t cpu_rmw_apply(cpu6502_t* cpu, cpu_rmw_t op, uint8_t v);

/** Updates N and Z from a value */
void cpu_update_nz(cpu6502_t* cpu, uint8_t val);
/** ADC / SBC (NMOS decimal mode included) and compare, without bus access */
void cpu_op_adc(cpu6502_t* cpu, uint8_t val);
void cpu_op_sbc(cpu6502_t* cpu, uint8_t val);
void cpu_op_cmp(cpu6502_t* cpu, uint8_t reg, uint8_t val);
/** LAX: A = X = value */
void cpu_op_lax(cpu6502_t* cpu, uint8_t v);
/**
 * @brief Actual value and destination of an « unstable » store (SHA/SHX/SHY/SHS)
 *
 * `base` = address before indexing, `addr` = corrected indexed address, `reg` = the
 * already-combined source. On a page crossing the emitted high byte is the
 * value itself: the destination is NOT `addr`.
 */
void cpu_sh_unstable(uint16_t base, uint16_t addr, uint8_t reg,
                     uint8_t* out_value, uint16_t* out_target);

/** --trace-irq traces, shared by both engines */
void cpu_irq_trace_entry(cpu6502_t* cpu, uint16_t pc_before);
void cpu_irq_trace_rti(cpu6502_t* cpu);

typedef struct opcode_info_s {
    const char* name;
    uint8_t cycles;
    uint8_t size;
    addressing_mode_t mode;
} opcode_info_t;

extern const opcode_info_t opcode_table[256];
int cpu_execute_opcode(cpu6502_t* cpu, uint8_t opcode);

#endif
