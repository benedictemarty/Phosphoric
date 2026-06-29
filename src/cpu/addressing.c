/**
 * @file addressing.c
 * @brief 6502 Addressing mode implementations
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.2.0-alpha
 */

#include "cpu/cpu_internal.h"
#include "memory/memory.h"

uint8_t cpu_mem_read(cpu6502_t* cpu, uint16_t addr) {
    uint8_t v = memory_read(cpu->memory, addr);
    cpu_tick(cpu, 1);   /* one bus cycle */
    return v;
}

void cpu_mem_write(cpu6502_t* cpu, uint16_t addr, uint8_t val) {
    memory_write(cpu->memory, addr, val);
    cpu_tick(cpu, 1);   /* one bus cycle */
}

uint8_t cpu_fetch_byte(cpu6502_t* cpu) {
    return cpu_mem_read(cpu, cpu->PC++);
}

uint16_t cpu_fetch_word_pc(cpu6502_t* cpu) {
    uint8_t lo = cpu_fetch_byte(cpu);
    uint8_t hi = cpu_fetch_byte(cpu);
    return (uint16_t)((hi << 8) | lo);
}

uint16_t addr_immediate(cpu6502_t* cpu) {
    return cpu->PC++;
}

uint16_t addr_zero_page(cpu6502_t* cpu) {
    return cpu_fetch_byte(cpu);
}

uint16_t addr_zero_page_x(cpu6502_t* cpu) {
    return (cpu_fetch_byte(cpu) + cpu->X) & 0xFF;
}

uint16_t addr_zero_page_y(cpu6502_t* cpu) {
    return (cpu_fetch_byte(cpu) + cpu->Y) & 0xFF;
}

uint16_t addr_absolute(cpu6502_t* cpu) {
    return cpu_fetch_word_pc(cpu);
}

uint16_t addr_absolute_x(cpu6502_t* cpu, bool* page_crossed) {
    uint16_t base = cpu_fetch_word_pc(cpu);
    uint16_t addr = base + cpu->X;
    if (page_crossed) *page_crossed = ((base & 0xFF00) != (addr & 0xFF00));
    return addr;
}

uint16_t addr_absolute_y(cpu6502_t* cpu, bool* page_crossed) {
    uint16_t base = cpu_fetch_word_pc(cpu);
    uint16_t addr = base + cpu->Y;
    if (page_crossed) *page_crossed = ((base & 0xFF00) != (addr & 0xFF00));
    return addr;
}

uint16_t addr_indirect(cpu6502_t* cpu) {
    uint16_t ptr = cpu_fetch_word_pc(cpu);
    uint8_t lo = cpu_mem_read(cpu, ptr);
    uint16_t ptr_hi = (ptr & 0xFF00) | ((ptr + 1) & 0x00FF);
    uint8_t hi = cpu_mem_read(cpu, ptr_hi);
    return (uint16_t)((hi << 8) | lo);
}

uint16_t addr_indexed_indirect(cpu6502_t* cpu) {
    uint8_t zpg = (cpu_fetch_byte(cpu) + cpu->X) & 0xFF;
    uint8_t lo = cpu_mem_read(cpu, zpg);
    uint8_t hi = cpu_mem_read(cpu, (zpg + 1) & 0xFF);
    return (uint16_t)((hi << 8) | lo);
}

uint16_t addr_indirect_indexed(cpu6502_t* cpu, bool* page_crossed) {
    uint8_t zpg = cpu_fetch_byte(cpu);
    uint8_t lo = cpu_mem_read(cpu, zpg);
    uint8_t hi = cpu_mem_read(cpu, (zpg + 1) & 0xFF);
    uint16_t base = (uint16_t)((hi << 8) | lo);
    uint16_t addr = base + cpu->Y;
    if (page_crossed) *page_crossed = ((base & 0xFF00) != (addr & 0xFF00));
    return addr;
}

uint16_t addr_relative(cpu6502_t* cpu) {
    int8_t offset = (int8_t)cpu_fetch_byte(cpu);
    return (uint16_t)(cpu->PC + offset);
}

void cpu_push(cpu6502_t* cpu, uint8_t val) {
    cpu_mem_write(cpu, 0x0100 + cpu->SP, val);
    cpu->SP--;
}

uint8_t cpu_pull(cpu6502_t* cpu) {
    cpu->SP++;
    return cpu_mem_read(cpu, 0x0100 + cpu->SP);
}

void cpu_push_word(cpu6502_t* cpu, uint16_t val) {
    cpu_push(cpu, (uint8_t)(val >> 8));
    cpu_push(cpu, (uint8_t)(val & 0xFF));
}

uint16_t cpu_pull_word(cpu6502_t* cpu) {
    uint8_t lo = cpu_pull(cpu);
    uint8_t hi = cpu_pull(cpu);
    return (uint16_t)((hi << 8) | lo);
}
