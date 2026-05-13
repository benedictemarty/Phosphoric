/**
 * @file cpu6502.c
 * @brief 6502 CPU core - fetch/decode/execute loop, interrupts, disassembly
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.2.0-alpha
 */

#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include <stdio.h>
#include <string.h>

void cpu_init(cpu6502_t* cpu, memory_t* memory) {
    memset(cpu, 0, sizeof(cpu6502_t));
    cpu->memory = memory;
    cpu->P = FLAG_UNUSED | FLAG_INTERRUPT;
}

void cpu_reset(cpu6502_t* cpu) {
    memory_t* mem = cpu->memory;
    cpu->PC = memory_read_word(mem, 0xFFFC);
    cpu->SP = 0xFD;
    cpu->P = FLAG_UNUSED | FLAG_INTERRUPT;
    cpu->cycles = 0;
    cpu->halted = false;
    cpu->nmi_pending = false;
    cpu->irq = 0;
}

static void handle_nmi(cpu6502_t* cpu) {
    cpu_push_word(cpu, cpu->PC);
    cpu_push(cpu, (cpu->P & ~FLAG_BREAK) | FLAG_UNUSED);
    cpu_set_flag(cpu, FLAG_INTERRUPT, true);
    cpu->PC = cpu_mem_read(cpu, 0xFFFA) | ((uint16_t)cpu_mem_read(cpu, 0xFFFB) << 8);
    cpu->nmi_pending = false;
    cpu->cycles += 7;
}

static void handle_irq(cpu6502_t* cpu) {
    uint16_t pc_before = cpu->PC;
    cpu_push_word(cpu, cpu->PC);
    cpu_push(cpu, (cpu->P & ~FLAG_BREAK) | FLAG_UNUSED);
    cpu_set_flag(cpu, FLAG_INTERRUPT, true);
    cpu->PC = cpu_mem_read(cpu, 0xFFFE) | ((uint16_t)cpu_mem_read(cpu, 0xFFFF) << 8);
    /* Level-triggered: do NOT clear cpu->irq here.
     * The I flag prevents re-entry. The source must deassert
     * its IRQ bit when the CPU acknowledges it (e.g. reading VIA IFR). */
    cpu->cycles += 7;

    if (cpu->irq_trace_fp) {
        /* VIA registers at $0300-$030F : IFR=$0D, IER=$0E. Read via memory
         * to capture current state at IRQ entry. */
        uint8_t ifr = cpu_mem_read(cpu, 0x030D);
        uint8_t ier = cpu_mem_read(cpu, 0x030E);
        fprintf((FILE*)cpu->irq_trace_fp,
                "%010llu IRQ-ENTRY PC_pre=$%04X target=$%04X IFR=$%02X IER=$%02X srcmask=$%02X\n",
                (unsigned long long)cpu->cycles, pc_before, cpu->PC, ifr, ier, cpu->irq);
        cpu->irq_trace_count++;
    }
}

int cpu_step(cpu6502_t* cpu) {
    if (cpu->halted) return 0;

    /* Handle interrupts */
    if (cpu->nmi_pending) {
        handle_nmi(cpu);
        return 7;
    }
    if (cpu->irq && !cpu_get_flag(cpu, FLAG_INTERRUPT)) {
        handle_irq(cpu);
        return 7;
    }

    /* Fetch opcode */
    uint8_t opcode = cpu_fetch_byte(cpu);

    /* Decode and execute */
    int cyc = cpu_execute_opcode(cpu, opcode);

    cpu->cycles += cyc;
    return cyc;
}

int cpu_execute_cycles(cpu6502_t* cpu, int cycles) {
    int executed = 0;
    while (executed < cycles && !cpu->halted) {
        executed += cpu_step(cpu);
    }
    return executed;
}

void cpu_nmi(cpu6502_t* cpu) {
    cpu->nmi_pending = true;
}

void cpu_irq_set(cpu6502_t* cpu, cpu_irq_source_t source) {
    cpu->irq |= (uint8_t)source;
}

void cpu_irq_clear(cpu6502_t* cpu, cpu_irq_source_t source) {
    cpu->irq &= ~(uint8_t)source;
}

void cpu_irq(cpu6502_t* cpu) {
    /* Legacy: assert VIA IRQ source for backward compatibility */
    cpu->irq |= IRQF_VIA;
}

void cpu_set_flag(cpu6502_t* cpu, cpu_flags_t flag, bool value) {
    if (value) cpu->P |= flag;
    else cpu->P &= ~flag;
}

bool cpu_get_flag(const cpu6502_t* cpu, cpu_flags_t flag) {
    return (cpu->P & flag) != 0;
}

/* ─── Disassembler ─── */
static const char* addr_mode_fmt(addressing_mode_t mode, uint8_t lo, uint8_t hi) {
    static char buf[32];
    uint16_t addr16 = (hi << 8) | lo;
    switch (mode) {
    case ADDR_IMPLICIT:         buf[0] = '\0'; break;
    case ADDR_ACCUMULATOR:      snprintf(buf, sizeof(buf), "A"); break;
    case ADDR_IMMEDIATE:        snprintf(buf, sizeof(buf), "#$%02X", lo); break;
    case ADDR_ZERO_PAGE:        snprintf(buf, sizeof(buf), "$%02X", lo); break;
    case ADDR_ZERO_PAGE_X:      snprintf(buf, sizeof(buf), "$%02X,X", lo); break;
    case ADDR_ZERO_PAGE_Y:      snprintf(buf, sizeof(buf), "$%02X,Y", lo); break;
    case ADDR_RELATIVE:         snprintf(buf, sizeof(buf), "$%04X", addr16); break;
    case ADDR_ABSOLUTE:         snprintf(buf, sizeof(buf), "$%04X", addr16); break;
    case ADDR_ABSOLUTE_X:       snprintf(buf, sizeof(buf), "$%04X,X", addr16); break;
    case ADDR_ABSOLUTE_Y:       snprintf(buf, sizeof(buf), "$%04X,Y", addr16); break;
    case ADDR_INDIRECT:         snprintf(buf, sizeof(buf), "($%04X)", addr16); break;
    case ADDR_INDEXED_INDIRECT: snprintf(buf, sizeof(buf), "($%02X,X)", lo); break;
    case ADDR_INDIRECT_INDEXED: snprintf(buf, sizeof(buf), "($%02X),Y", lo); break;
    }
    return buf;
}

int cpu_disassemble(const cpu6502_t* cpu, uint16_t address, char* buffer, size_t buffer_size) {
    memory_t* mem = cpu->memory;
    uint8_t opcode = memory_read(mem, address);
    const opcode_info_t* info = &opcode_table[opcode];

    uint8_t lo = 0, hi = 0;
    if (info->size >= 2) lo = memory_read(mem, address + 1);
    if (info->size >= 3) hi = memory_read(mem, address + 2);

    /* For relative mode, compute target address */
    if (info->mode == ADDR_RELATIVE) {
        int8_t offset = (int8_t)lo;
        uint16_t target = address + 2 + offset;
        lo = target & 0xFF;
        hi = target >> 8;
    }

    const char* operand = addr_mode_fmt(info->mode, lo, hi);
    snprintf(buffer, buffer_size, "%s %s", info->name, operand);
    return info->size;
}

void cpu_get_state_string(const cpu6502_t* cpu, char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size,
             "A:%02X X:%02X Y:%02X SP:%02X P:%c%c%c%c%c%c%c%c PC:%04X CYC:%llu",
             cpu->A, cpu->X, cpu->Y, cpu->SP,
             (cpu->P & FLAG_NEGATIVE)  ? 'N' : '.',
             (cpu->P & FLAG_OVERFLOW)  ? 'V' : '.',
             (cpu->P & FLAG_UNUSED)    ? '-' : '.',
             (cpu->P & FLAG_BREAK)     ? 'B' : '.',
             (cpu->P & FLAG_DECIMAL)   ? 'D' : '.',
             (cpu->P & FLAG_INTERRUPT) ? 'I' : '.',
             (cpu->P & FLAG_ZERO)      ? 'Z' : '.',
             (cpu->P & FLAG_CARRY)     ? 'C' : '.',
             cpu->PC,
             (unsigned long long)cpu->cycles);
}
