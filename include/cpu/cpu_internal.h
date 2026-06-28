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

typedef struct opcode_info_s {
    const char* name;
    uint8_t cycles;
    uint8_t size;
    addressing_mode_t mode;
} opcode_info_t;

extern const opcode_info_t opcode_table[256];
int cpu_execute_opcode(cpu6502_t* cpu, uint8_t opcode);

#endif
