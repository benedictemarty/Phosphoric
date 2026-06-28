/**
 * @file opcodes.c
 * @brief Complete 6502 opcode implementations (151 official + 105 illegal/undocumented)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.3.0-alpha
 *
 * The MOS 6502 used in the ORIC-1/Atmos is the NMOS variant: all 256 opcodes
 * decode to *something*. The 105 "illegal" opcodes are reproduced here with
 * their documented NMOS behaviour (combined RMW+ALU ops, LAX/SAX, the
 * immediate-AND family, the unstable SH/ANE/LXA group, multi-byte NOPs that
 * still perform a dummy bus read, and the JAM/KIL opcodes that freeze the CPU).
 */

#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include <string.h>
#include <stdio.h>

/* Helper: update N and Z flags based on value */
static inline void update_nz(cpu6502_t* cpu, uint8_t val) {
    cpu_set_flag(cpu, FLAG_ZERO, val == 0);
    cpu_set_flag(cpu, FLAG_NEGATIVE, (val & 0x80) != 0);
}

/* ─── Opcode table: name, base_cycles, size, addressing_mode ─── */
const opcode_info_t opcode_table[256] = {
    /* 0x00 */ {"BRK",7,1,ADDR_IMPLICIT},    {"ORA",6,2,ADDR_INDEXED_INDIRECT},
    /* 0x02 */ {"JAM",2,1,ADDR_IMPLICIT},     {"SLO",8,2,ADDR_INDEXED_INDIRECT},
    /* 0x04 */ {"NOP",3,2,ADDR_ZERO_PAGE},    {"ORA",3,2,ADDR_ZERO_PAGE},
    /* 0x06 */ {"ASL",5,2,ADDR_ZERO_PAGE},    {"SLO",5,2,ADDR_ZERO_PAGE},
    /* 0x08 */ {"PHP",3,1,ADDR_IMPLICIT},     {"ORA",2,2,ADDR_IMMEDIATE},
    /* 0x0A */ {"ASL",2,1,ADDR_ACCUMULATOR},  {"ANC",2,2,ADDR_IMMEDIATE},
    /* 0x0C */ {"NOP",4,3,ADDR_ABSOLUTE},     {"ORA",4,3,ADDR_ABSOLUTE},
    /* 0x0E */ {"ASL",6,3,ADDR_ABSOLUTE},     {"SLO",6,3,ADDR_ABSOLUTE},

    /* 0x10 */ {"BPL",2,2,ADDR_RELATIVE},     {"ORA",5,2,ADDR_INDIRECT_INDEXED},
    /* 0x12 */ {"JAM",2,1,ADDR_IMPLICIT},     {"SLO",8,2,ADDR_INDIRECT_INDEXED},
    /* 0x14 */ {"NOP",4,2,ADDR_ZERO_PAGE_X},  {"ORA",4,2,ADDR_ZERO_PAGE_X},
    /* 0x16 */ {"ASL",6,2,ADDR_ZERO_PAGE_X},  {"SLO",6,2,ADDR_ZERO_PAGE_X},
    /* 0x18 */ {"CLC",2,1,ADDR_IMPLICIT},     {"ORA",4,3,ADDR_ABSOLUTE_Y},
    /* 0x1A */ {"NOP",2,1,ADDR_IMPLICIT},     {"SLO",7,3,ADDR_ABSOLUTE_Y},
    /* 0x1C */ {"NOP",4,3,ADDR_ABSOLUTE_X},   {"ORA",4,3,ADDR_ABSOLUTE_X},
    /* 0x1E */ {"ASL",7,3,ADDR_ABSOLUTE_X},   {"SLO",7,3,ADDR_ABSOLUTE_X},

    /* 0x20 */ {"JSR",6,3,ADDR_ABSOLUTE},     {"AND",6,2,ADDR_INDEXED_INDIRECT},
    /* 0x22 */ {"JAM",2,1,ADDR_IMPLICIT},     {"RLA",8,2,ADDR_INDEXED_INDIRECT},
    /* 0x24 */ {"BIT",3,2,ADDR_ZERO_PAGE},    {"AND",3,2,ADDR_ZERO_PAGE},
    /* 0x26 */ {"ROL",5,2,ADDR_ZERO_PAGE},    {"RLA",5,2,ADDR_ZERO_PAGE},
    /* 0x28 */ {"PLP",4,1,ADDR_IMPLICIT},     {"AND",2,2,ADDR_IMMEDIATE},
    /* 0x2A */ {"ROL",2,1,ADDR_ACCUMULATOR},  {"ANC",2,2,ADDR_IMMEDIATE},
    /* 0x2C */ {"BIT",4,3,ADDR_ABSOLUTE},     {"AND",4,3,ADDR_ABSOLUTE},
    /* 0x2E */ {"ROL",6,3,ADDR_ABSOLUTE},     {"RLA",6,3,ADDR_ABSOLUTE},

    /* 0x30 */ {"BMI",2,2,ADDR_RELATIVE},     {"AND",5,2,ADDR_INDIRECT_INDEXED},
    /* 0x32 */ {"JAM",2,1,ADDR_IMPLICIT},     {"RLA",8,2,ADDR_INDIRECT_INDEXED},
    /* 0x34 */ {"NOP",4,2,ADDR_ZERO_PAGE_X},  {"AND",4,2,ADDR_ZERO_PAGE_X},
    /* 0x36 */ {"ROL",6,2,ADDR_ZERO_PAGE_X},  {"RLA",6,2,ADDR_ZERO_PAGE_X},
    /* 0x38 */ {"SEC",2,1,ADDR_IMPLICIT},     {"AND",4,3,ADDR_ABSOLUTE_Y},
    /* 0x3A */ {"NOP",2,1,ADDR_IMPLICIT},     {"RLA",7,3,ADDR_ABSOLUTE_Y},
    /* 0x3C */ {"NOP",4,3,ADDR_ABSOLUTE_X},   {"AND",4,3,ADDR_ABSOLUTE_X},
    /* 0x3E */ {"ROL",7,3,ADDR_ABSOLUTE_X},   {"RLA",7,3,ADDR_ABSOLUTE_X},

    /* 0x40 */ {"RTI",6,1,ADDR_IMPLICIT},     {"EOR",6,2,ADDR_INDEXED_INDIRECT},
    /* 0x42 */ {"JAM",2,1,ADDR_IMPLICIT},     {"SRE",8,2,ADDR_INDEXED_INDIRECT},
    /* 0x44 */ {"NOP",3,2,ADDR_ZERO_PAGE},    {"EOR",3,2,ADDR_ZERO_PAGE},
    /* 0x46 */ {"LSR",5,2,ADDR_ZERO_PAGE},    {"SRE",5,2,ADDR_ZERO_PAGE},
    /* 0x48 */ {"PHA",3,1,ADDR_IMPLICIT},     {"EOR",2,2,ADDR_IMMEDIATE},
    /* 0x4A */ {"LSR",2,1,ADDR_ACCUMULATOR},  {"ALR",2,2,ADDR_IMMEDIATE},
    /* 0x4C */ {"JMP",3,3,ADDR_ABSOLUTE},     {"EOR",4,3,ADDR_ABSOLUTE},
    /* 0x4E */ {"LSR",6,3,ADDR_ABSOLUTE},     {"SRE",6,3,ADDR_ABSOLUTE},

    /* 0x50 */ {"BVC",2,2,ADDR_RELATIVE},     {"EOR",5,2,ADDR_INDIRECT_INDEXED},
    /* 0x52 */ {"JAM",2,1,ADDR_IMPLICIT},     {"SRE",8,2,ADDR_INDIRECT_INDEXED},
    /* 0x54 */ {"NOP",4,2,ADDR_ZERO_PAGE_X},  {"EOR",4,2,ADDR_ZERO_PAGE_X},
    /* 0x56 */ {"LSR",6,2,ADDR_ZERO_PAGE_X},  {"SRE",6,2,ADDR_ZERO_PAGE_X},
    /* 0x58 */ {"CLI",2,1,ADDR_IMPLICIT},     {"EOR",4,3,ADDR_ABSOLUTE_Y},
    /* 0x5A */ {"NOP",2,1,ADDR_IMPLICIT},     {"SRE",7,3,ADDR_ABSOLUTE_Y},
    /* 0x5C */ {"NOP",4,3,ADDR_ABSOLUTE_X},   {"EOR",4,3,ADDR_ABSOLUTE_X},
    /* 0x5E */ {"LSR",7,3,ADDR_ABSOLUTE_X},   {"SRE",7,3,ADDR_ABSOLUTE_X},

    /* 0x60 */ {"RTS",6,1,ADDR_IMPLICIT},     {"ADC",6,2,ADDR_INDEXED_INDIRECT},
    /* 0x62 */ {"JAM",2,1,ADDR_IMPLICIT},     {"RRA",8,2,ADDR_INDEXED_INDIRECT},
    /* 0x64 */ {"NOP",3,2,ADDR_ZERO_PAGE},    {"ADC",3,2,ADDR_ZERO_PAGE},
    /* 0x66 */ {"ROR",5,2,ADDR_ZERO_PAGE},    {"RRA",5,2,ADDR_ZERO_PAGE},
    /* 0x68 */ {"PLA",4,1,ADDR_IMPLICIT},     {"ADC",2,2,ADDR_IMMEDIATE},
    /* 0x6A */ {"ROR",2,1,ADDR_ACCUMULATOR},  {"ARR",2,2,ADDR_IMMEDIATE},
    /* 0x6C */ {"JMP",5,3,ADDR_INDIRECT},     {"ADC",4,3,ADDR_ABSOLUTE},
    /* 0x6E */ {"ROR",6,3,ADDR_ABSOLUTE},     {"RRA",6,3,ADDR_ABSOLUTE},

    /* 0x70 */ {"BVS",2,2,ADDR_RELATIVE},     {"ADC",5,2,ADDR_INDIRECT_INDEXED},
    /* 0x72 */ {"JAM",2,1,ADDR_IMPLICIT},     {"RRA",8,2,ADDR_INDIRECT_INDEXED},
    /* 0x74 */ {"NOP",4,2,ADDR_ZERO_PAGE_X},  {"ADC",4,2,ADDR_ZERO_PAGE_X},
    /* 0x76 */ {"ROR",6,2,ADDR_ZERO_PAGE_X},  {"RRA",6,2,ADDR_ZERO_PAGE_X},
    /* 0x78 */ {"SEI",2,1,ADDR_IMPLICIT},     {"ADC",4,3,ADDR_ABSOLUTE_Y},
    /* 0x7A */ {"NOP",2,1,ADDR_IMPLICIT},     {"RRA",7,3,ADDR_ABSOLUTE_Y},
    /* 0x7C */ {"NOP",4,3,ADDR_ABSOLUTE_X},   {"ADC",4,3,ADDR_ABSOLUTE_X},
    /* 0x7E */ {"ROR",7,3,ADDR_ABSOLUTE_X},   {"RRA",7,3,ADDR_ABSOLUTE_X},

    /* 0x80 */ {"NOP",2,2,ADDR_IMMEDIATE},    {"STA",6,2,ADDR_INDEXED_INDIRECT},
    /* 0x82 */ {"NOP",2,2,ADDR_IMMEDIATE},    {"SAX",6,2,ADDR_INDEXED_INDIRECT},
    /* 0x84 */ {"STY",3,2,ADDR_ZERO_PAGE},    {"STA",3,2,ADDR_ZERO_PAGE},
    /* 0x86 */ {"STX",3,2,ADDR_ZERO_PAGE},    {"SAX",3,2,ADDR_ZERO_PAGE},
    /* 0x88 */ {"DEY",2,1,ADDR_IMPLICIT},     {"NOP",2,2,ADDR_IMMEDIATE},
    /* 0x8A */ {"TXA",2,1,ADDR_IMPLICIT},     {"ANE",2,2,ADDR_IMMEDIATE},
    /* 0x8C */ {"STY",4,3,ADDR_ABSOLUTE},     {"STA",4,3,ADDR_ABSOLUTE},
    /* 0x8E */ {"STX",4,3,ADDR_ABSOLUTE},     {"SAX",4,3,ADDR_ABSOLUTE},

    /* 0x90 */ {"BCC",2,2,ADDR_RELATIVE},     {"STA",6,2,ADDR_INDIRECT_INDEXED},
    /* 0x92 */ {"JAM",2,1,ADDR_IMPLICIT},     {"SHA",6,2,ADDR_INDIRECT_INDEXED},
    /* 0x94 */ {"STY",4,2,ADDR_ZERO_PAGE_X},  {"STA",4,2,ADDR_ZERO_PAGE_X},
    /* 0x96 */ {"STX",4,2,ADDR_ZERO_PAGE_Y},  {"SAX",4,2,ADDR_ZERO_PAGE_Y},
    /* 0x98 */ {"TYA",2,1,ADDR_IMPLICIT},     {"STA",5,3,ADDR_ABSOLUTE_Y},
    /* 0x9A */ {"TXS",2,1,ADDR_IMPLICIT},     {"TAS",5,3,ADDR_ABSOLUTE_Y},
    /* 0x9C */ {"SHY",5,3,ADDR_ABSOLUTE_X},   {"STA",5,3,ADDR_ABSOLUTE_X},
    /* 0x9E */ {"SHX",5,3,ADDR_ABSOLUTE_Y},   {"SHA",5,3,ADDR_ABSOLUTE_Y},

    /* 0xA0 */ {"LDY",2,2,ADDR_IMMEDIATE},    {"LDA",6,2,ADDR_INDEXED_INDIRECT},
    /* 0xA2 */ {"LDX",2,2,ADDR_IMMEDIATE},    {"LAX",6,2,ADDR_INDEXED_INDIRECT},
    /* 0xA4 */ {"LDY",3,2,ADDR_ZERO_PAGE},    {"LDA",3,2,ADDR_ZERO_PAGE},
    /* 0xA6 */ {"LDX",3,2,ADDR_ZERO_PAGE},    {"LAX",3,2,ADDR_ZERO_PAGE},
    /* 0xA8 */ {"TAY",2,1,ADDR_IMPLICIT},     {"LDA",2,2,ADDR_IMMEDIATE},
    /* 0xAA */ {"TAX",2,1,ADDR_IMPLICIT},     {"LXA",2,2,ADDR_IMMEDIATE},
    /* 0xAC */ {"LDY",4,3,ADDR_ABSOLUTE},     {"LDA",4,3,ADDR_ABSOLUTE},
    /* 0xAE */ {"LDX",4,3,ADDR_ABSOLUTE},     {"LAX",4,3,ADDR_ABSOLUTE},

    /* 0xB0 */ {"BCS",2,2,ADDR_RELATIVE},     {"LDA",5,2,ADDR_INDIRECT_INDEXED},
    /* 0xB2 */ {"JAM",2,1,ADDR_IMPLICIT},     {"LAX",5,2,ADDR_INDIRECT_INDEXED},
    /* 0xB4 */ {"LDY",4,2,ADDR_ZERO_PAGE_X},  {"LDA",4,2,ADDR_ZERO_PAGE_X},
    /* 0xB6 */ {"LDX",4,2,ADDR_ZERO_PAGE_Y},  {"LAX",4,2,ADDR_ZERO_PAGE_Y},
    /* 0xB8 */ {"CLV",2,1,ADDR_IMPLICIT},     {"LDA",4,3,ADDR_ABSOLUTE_Y},
    /* 0xBA */ {"TSX",2,1,ADDR_IMPLICIT},     {"LAS",4,3,ADDR_ABSOLUTE_Y},
    /* 0xBC */ {"LDY",4,3,ADDR_ABSOLUTE_X},   {"LDA",4,3,ADDR_ABSOLUTE_X},
    /* 0xBE */ {"LDX",4,3,ADDR_ABSOLUTE_Y},   {"LAX",4,3,ADDR_ABSOLUTE_Y},

    /* 0xC0 */ {"CPY",2,2,ADDR_IMMEDIATE},    {"CMP",6,2,ADDR_INDEXED_INDIRECT},
    /* 0xC2 */ {"NOP",2,2,ADDR_IMMEDIATE},    {"DCP",8,2,ADDR_INDEXED_INDIRECT},
    /* 0xC4 */ {"CPY",3,2,ADDR_ZERO_PAGE},    {"CMP",3,2,ADDR_ZERO_PAGE},
    /* 0xC6 */ {"DEC",5,2,ADDR_ZERO_PAGE},    {"DCP",5,2,ADDR_ZERO_PAGE},
    /* 0xC8 */ {"INY",2,1,ADDR_IMPLICIT},     {"CMP",2,2,ADDR_IMMEDIATE},
    /* 0xCA */ {"DEX",2,1,ADDR_IMPLICIT},     {"SBX",2,2,ADDR_IMMEDIATE},
    /* 0xCC */ {"CPY",4,3,ADDR_ABSOLUTE},     {"CMP",4,3,ADDR_ABSOLUTE},
    /* 0xCE */ {"DEC",6,3,ADDR_ABSOLUTE},     {"DCP",6,3,ADDR_ABSOLUTE},

    /* 0xD0 */ {"BNE",2,2,ADDR_RELATIVE},     {"CMP",5,2,ADDR_INDIRECT_INDEXED},
    /* 0xD2 */ {"JAM",2,1,ADDR_IMPLICIT},     {"DCP",8,2,ADDR_INDIRECT_INDEXED},
    /* 0xD4 */ {"NOP",4,2,ADDR_ZERO_PAGE_X},  {"CMP",4,2,ADDR_ZERO_PAGE_X},
    /* 0xD6 */ {"DEC",6,2,ADDR_ZERO_PAGE_X},  {"DCP",6,2,ADDR_ZERO_PAGE_X},
    /* 0xD8 */ {"CLD",2,1,ADDR_IMPLICIT},     {"CMP",4,3,ADDR_ABSOLUTE_Y},
    /* 0xDA */ {"NOP",2,1,ADDR_IMPLICIT},     {"DCP",7,3,ADDR_ABSOLUTE_Y},
    /* 0xDC */ {"NOP",4,3,ADDR_ABSOLUTE_X},   {"CMP",4,3,ADDR_ABSOLUTE_X},
    /* 0xDE */ {"DEC",7,3,ADDR_ABSOLUTE_X},   {"DCP",7,3,ADDR_ABSOLUTE_X},

    /* 0xE0 */ {"CPX",2,2,ADDR_IMMEDIATE},    {"SBC",6,2,ADDR_INDEXED_INDIRECT},
    /* 0xE2 */ {"NOP",2,2,ADDR_IMMEDIATE},    {"ISC",8,2,ADDR_INDEXED_INDIRECT},
    /* 0xE4 */ {"CPX",3,2,ADDR_ZERO_PAGE},    {"SBC",3,2,ADDR_ZERO_PAGE},
    /* 0xE6 */ {"INC",5,2,ADDR_ZERO_PAGE},    {"ISC",5,2,ADDR_ZERO_PAGE},
    /* 0xE8 */ {"INX",2,1,ADDR_IMPLICIT},     {"SBC",2,2,ADDR_IMMEDIATE},
    /* 0xEA */ {"NOP",2,1,ADDR_IMPLICIT},     {"SBC",2,2,ADDR_IMMEDIATE},
    /* 0xEC */ {"CPX",4,3,ADDR_ABSOLUTE},     {"SBC",4,3,ADDR_ABSOLUTE},
    /* 0xEE */ {"INC",6,3,ADDR_ABSOLUTE},     {"ISC",6,3,ADDR_ABSOLUTE},

    /* 0xF0 */ {"BEQ",2,2,ADDR_RELATIVE},     {"SBC",5,2,ADDR_INDIRECT_INDEXED},
    /* 0xF2 */ {"JAM",2,1,ADDR_IMPLICIT},     {"ISC",8,2,ADDR_INDIRECT_INDEXED},
    /* 0xF4 */ {"NOP",4,2,ADDR_ZERO_PAGE_X},  {"SBC",4,2,ADDR_ZERO_PAGE_X},
    /* 0xF6 */ {"INC",6,2,ADDR_ZERO_PAGE_X},  {"ISC",6,2,ADDR_ZERO_PAGE_X},
    /* 0xF8 */ {"SED",2,1,ADDR_IMPLICIT},     {"SBC",4,3,ADDR_ABSOLUTE_Y},
    /* 0xFA */ {"NOP",2,1,ADDR_IMPLICIT},     {"ISC",7,3,ADDR_ABSOLUTE_Y},
    /* 0xFC */ {"NOP",4,3,ADDR_ABSOLUTE_X},   {"SBC",4,3,ADDR_ABSOLUTE_X},
    /* 0xFE */ {"INC",7,3,ADDR_ABSOLUTE_X},   {"ISC",7,3,ADDR_ABSOLUTE_X},
};

/* ─── ADC: Add with Carry ─── */
static void op_adc(cpu6502_t* cpu, uint8_t val) {
    if (cpu_get_flag(cpu, FLAG_DECIMAL)) {
        /* BCD mode */
        uint16_t lo = (cpu->A & 0x0F) + (val & 0x0F) + (cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0);
        uint16_t hi = (cpu->A & 0xF0) + (val & 0xF0);
        if (lo > 0x09) { lo += 0x06; hi += 0x10; }
        /* Overflow uses binary result */
        uint16_t bin = (uint16_t)cpu->A + (uint16_t)val + (cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0);
        cpu_set_flag(cpu, FLAG_OVERFLOW, (~(cpu->A ^ val) & (cpu->A ^ (uint8_t)bin) & 0x80) != 0);
        uint16_t sum = hi + (lo & 0x0F);
        if (sum > 0x9F) sum += 0x60;
        cpu_set_flag(cpu, FLAG_CARRY, sum > 0xFF);
        cpu->A = (uint8_t)sum;
        cpu_set_flag(cpu, FLAG_NEGATIVE, (cpu->A & 0x80) != 0);
        cpu_set_flag(cpu, FLAG_ZERO, (uint8_t)bin == 0);
    } else {
        uint16_t sum = (uint16_t)cpu->A + (uint16_t)val + (cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0);
        cpu_set_flag(cpu, FLAG_CARRY, sum > 0xFF);
        cpu_set_flag(cpu, FLAG_OVERFLOW, (~(cpu->A ^ val) & (cpu->A ^ (uint8_t)sum) & 0x80) != 0);
        cpu->A = (uint8_t)sum;
        update_nz(cpu, cpu->A);
    }
}

/* ─── SBC: Subtract with Carry ─── */
static void op_sbc(cpu6502_t* cpu, uint8_t val) {
    if (cpu_get_flag(cpu, FLAG_DECIMAL)) {
        uint16_t bin = (uint16_t)cpu->A - (uint16_t)val - (cpu_get_flag(cpu, FLAG_CARRY) ? 0 : 1);
        int16_t lo = (cpu->A & 0x0F) - (val & 0x0F) - (cpu_get_flag(cpu, FLAG_CARRY) ? 0 : 1);
        int16_t hi = (cpu->A & 0xF0) - (val & 0xF0);
        if (lo < 0) { lo -= 0x06; hi -= 0x10; }
        int16_t sum = hi + (lo & 0x0F);
        if (sum < 0) sum -= 0x60;
        cpu_set_flag(cpu, FLAG_CARRY, bin < 0x100);
        cpu_set_flag(cpu, FLAG_OVERFLOW, ((cpu->A ^ val) & (cpu->A ^ (uint8_t)bin) & 0x80) != 0);
        cpu->A = (uint8_t)sum;
        cpu_set_flag(cpu, FLAG_NEGATIVE, (cpu->A & 0x80) != 0);
        cpu_set_flag(cpu, FLAG_ZERO, (uint8_t)bin == 0);
    } else {
        uint16_t diff = (uint16_t)cpu->A - (uint16_t)val - (cpu_get_flag(cpu, FLAG_CARRY) ? 0 : 1);
        cpu_set_flag(cpu, FLAG_CARRY, diff < 0x100);
        cpu_set_flag(cpu, FLAG_OVERFLOW, ((cpu->A ^ val) & (cpu->A ^ (uint8_t)diff) & 0x80) != 0);
        cpu->A = (uint8_t)diff;
        update_nz(cpu, cpu->A);
    }
}

/* ─── Compare helper ─── */
static void op_cmp(cpu6502_t* cpu, uint8_t reg, uint8_t val) {
    uint16_t diff = (uint16_t)reg - (uint16_t)val;
    cpu_set_flag(cpu, FLAG_CARRY, reg >= val);
    update_nz(cpu, (uint8_t)diff);
}

/* ─── Branch helper ─── */
static int do_branch(cpu6502_t* cpu, bool condition) {
    uint16_t target = addr_relative(cpu);
    if (condition) {
        int extra = ((cpu->PC & 0xFF00) != (target & 0xFF00)) ? 2 : 1;
        cpu->PC = target;
        return extra;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Illegal / undocumented opcode helpers (NMOS 6502)
 *
 * The combined read-modify-write + ALU forms (SLO/RLA/SRE/RRA/DCP/ISC)
 * perform the memory operation first, then fold the modified value into A
 * (or compare against it) reusing the official ALU helpers above. This
 * matches the documented NMOS behaviour and the flag side effects.
 * ════════════════════════════════════════════════════════════════════ */

/* SLO (ASO): mem <<= 1 (carry from bit7), then A |= mem */
static void op_slo(cpu6502_t* cpu, uint16_t addr) {
    uint8_t v = cpu_mem_read(cpu, addr);
    cpu_set_flag(cpu, FLAG_CARRY, (v & 0x80) != 0);
    v <<= 1;
    cpu_mem_write(cpu, addr, v);
    cpu->A |= v;
    update_nz(cpu, cpu->A);
}

/* RLA: mem = ROL(mem), then A &= mem */
static void op_rla(cpu6502_t* cpu, uint16_t addr) {
    uint8_t v = cpu_mem_read(cpu, addr);
    uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0;
    cpu_set_flag(cpu, FLAG_CARRY, (v & 0x80) != 0);
    v = (uint8_t)((v << 1) | c);
    cpu_mem_write(cpu, addr, v);
    cpu->A &= v;
    update_nz(cpu, cpu->A);
}

/* SRE (LSE): mem >>= 1 (carry from bit0), then A ^= mem */
static void op_sre(cpu6502_t* cpu, uint16_t addr) {
    uint8_t v = cpu_mem_read(cpu, addr);
    cpu_set_flag(cpu, FLAG_CARRY, (v & 0x01) != 0);
    v >>= 1;
    cpu_mem_write(cpu, addr, v);
    cpu->A ^= v;
    update_nz(cpu, cpu->A);
}

/* RRA: mem = ROR(mem), then ADC mem */
static void op_rra(cpu6502_t* cpu, uint16_t addr) {
    uint8_t v = cpu_mem_read(cpu, addr);
    uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
    cpu_set_flag(cpu, FLAG_CARRY, (v & 0x01) != 0);
    v = (uint8_t)((v >> 1) | c);
    cpu_mem_write(cpu, addr, v);
    op_adc(cpu, v);
}

/* DCP (DCM): mem -= 1, then CMP A with mem */
static void op_dcp(cpu6502_t* cpu, uint16_t addr) {
    uint8_t v = (uint8_t)(cpu_mem_read(cpu, addr) - 1);
    cpu_mem_write(cpu, addr, v);
    op_cmp(cpu, cpu->A, v);
}

/* ISC (ISB/INS): mem += 1, then SBC mem */
static void op_isc(cpu6502_t* cpu, uint16_t addr) {
    uint8_t v = (uint8_t)(cpu_mem_read(cpu, addr) + 1);
    cpu_mem_write(cpu, addr, v);
    op_sbc(cpu, v);
}

/* LAX: A = X = mem */
static void op_lax(cpu6502_t* cpu, uint8_t v) {
    cpu->A = v;
    cpu->X = v;
    update_nz(cpu, v);
}

/* ─── Execute a single opcode. Returns total cycles used. ─── */
int cpu_execute_opcode(cpu6502_t* cpu, uint8_t opcode) {
    int cycles = opcode_table[opcode].cycles;
    int extra = 0;
    bool page_crossed = false;
    uint16_t addr;
    uint8_t val, result;

    switch (opcode) {
    /* ── LDA ── */
    case 0xA9: val = cpu_mem_read(cpu, addr_immediate(cpu)); cpu->A = val; update_nz(cpu, cpu->A); break;
    case 0xA5: val = cpu_mem_read(cpu, addr_zero_page(cpu)); cpu->A = val; update_nz(cpu, cpu->A); break;
    case 0xB5: val = cpu_mem_read(cpu, addr_zero_page_x(cpu)); cpu->A = val; update_nz(cpu, cpu->A); break;
    case 0xAD: val = cpu_mem_read(cpu, addr_absolute(cpu)); cpu->A = val; update_nz(cpu, cpu->A); break;
    case 0xBD: addr = addr_absolute_x(cpu, &page_crossed); val = cpu_mem_read(cpu, addr); cpu->A = val; update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0xB9: addr = addr_absolute_y(cpu, &page_crossed); val = cpu_mem_read(cpu, addr); cpu->A = val; update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0xA1: val = cpu_mem_read(cpu, addr_indexed_indirect(cpu)); cpu->A = val; update_nz(cpu, cpu->A); break;
    case 0xB1: addr = addr_indirect_indexed(cpu, &page_crossed); val = cpu_mem_read(cpu, addr); cpu->A = val; update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;

    /* ── LDX ── */
    case 0xA2: val = cpu_mem_read(cpu, addr_immediate(cpu)); cpu->X = val; update_nz(cpu, cpu->X); break;
    case 0xA6: val = cpu_mem_read(cpu, addr_zero_page(cpu)); cpu->X = val; update_nz(cpu, cpu->X); break;
    case 0xB6: val = cpu_mem_read(cpu, addr_zero_page_y(cpu)); cpu->X = val; update_nz(cpu, cpu->X); break;
    case 0xAE: val = cpu_mem_read(cpu, addr_absolute(cpu)); cpu->X = val; update_nz(cpu, cpu->X); break;
    case 0xBE: addr = addr_absolute_y(cpu, &page_crossed); val = cpu_mem_read(cpu, addr); cpu->X = val; update_nz(cpu, cpu->X); if(page_crossed) extra=1; break;

    /* ── LDY ── */
    case 0xA0: val = cpu_mem_read(cpu, addr_immediate(cpu)); cpu->Y = val; update_nz(cpu, cpu->Y); break;
    case 0xA4: val = cpu_mem_read(cpu, addr_zero_page(cpu)); cpu->Y = val; update_nz(cpu, cpu->Y); break;
    case 0xB4: val = cpu_mem_read(cpu, addr_zero_page_x(cpu)); cpu->Y = val; update_nz(cpu, cpu->Y); break;
    case 0xAC: val = cpu_mem_read(cpu, addr_absolute(cpu)); cpu->Y = val; update_nz(cpu, cpu->Y); break;
    case 0xBC: addr = addr_absolute_x(cpu, &page_crossed); val = cpu_mem_read(cpu, addr); cpu->Y = val; update_nz(cpu, cpu->Y); if(page_crossed) extra=1; break;

    /* ── STA ── */
    case 0x85: cpu_mem_write(cpu, addr_zero_page(cpu), cpu->A); break;
    case 0x95: cpu_mem_write(cpu, addr_zero_page_x(cpu), cpu->A); break;
    case 0x8D: cpu_mem_write(cpu, addr_absolute(cpu), cpu->A); break;
    case 0x9D: addr = addr_absolute_x(cpu, NULL); cpu_mem_write(cpu, addr, cpu->A); break;
    case 0x99: addr = addr_absolute_y(cpu, NULL); cpu_mem_write(cpu, addr, cpu->A); break;
    case 0x81: cpu_mem_write(cpu, addr_indexed_indirect(cpu), cpu->A); break;
    case 0x91: addr = addr_indirect_indexed(cpu, NULL); cpu_mem_write(cpu, addr, cpu->A); break;

    /* ── STX ── */
    case 0x86: cpu_mem_write(cpu, addr_zero_page(cpu), cpu->X); break;
    case 0x96: cpu_mem_write(cpu, addr_zero_page_y(cpu), cpu->X); break;
    case 0x8E: cpu_mem_write(cpu, addr_absolute(cpu), cpu->X); break;

    /* ── STY ── */
    case 0x84: cpu_mem_write(cpu, addr_zero_page(cpu), cpu->Y); break;
    case 0x94: cpu_mem_write(cpu, addr_zero_page_x(cpu), cpu->Y); break;
    case 0x8C: cpu_mem_write(cpu, addr_absolute(cpu), cpu->Y); break;

    /* ── ADC ── */
    case 0x69: op_adc(cpu, cpu_mem_read(cpu, addr_immediate(cpu))); break;
    case 0x65: op_adc(cpu, cpu_mem_read(cpu, addr_zero_page(cpu))); break;
    case 0x75: op_adc(cpu, cpu_mem_read(cpu, addr_zero_page_x(cpu))); break;
    case 0x6D: op_adc(cpu, cpu_mem_read(cpu, addr_absolute(cpu))); break;
    case 0x7D: addr = addr_absolute_x(cpu, &page_crossed); op_adc(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;
    case 0x79: addr = addr_absolute_y(cpu, &page_crossed); op_adc(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;
    case 0x61: op_adc(cpu, cpu_mem_read(cpu, addr_indexed_indirect(cpu))); break;
    case 0x71: addr = addr_indirect_indexed(cpu, &page_crossed); op_adc(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;

    /* ── SBC ── */
    case 0xE9: op_sbc(cpu, cpu_mem_read(cpu, addr_immediate(cpu))); break;
    case 0xEB: op_sbc(cpu, cpu_mem_read(cpu, addr_immediate(cpu))); break; /* unofficial SBC */
    case 0xE5: op_sbc(cpu, cpu_mem_read(cpu, addr_zero_page(cpu))); break;
    case 0xF5: op_sbc(cpu, cpu_mem_read(cpu, addr_zero_page_x(cpu))); break;
    case 0xED: op_sbc(cpu, cpu_mem_read(cpu, addr_absolute(cpu))); break;
    case 0xFD: addr = addr_absolute_x(cpu, &page_crossed); op_sbc(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;
    case 0xF9: addr = addr_absolute_y(cpu, &page_crossed); op_sbc(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;
    case 0xE1: op_sbc(cpu, cpu_mem_read(cpu, addr_indexed_indirect(cpu))); break;
    case 0xF1: addr = addr_indirect_indexed(cpu, &page_crossed); op_sbc(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;

    /* ── AND ── */
    case 0x29: cpu->A &= cpu_mem_read(cpu, addr_immediate(cpu)); update_nz(cpu, cpu->A); break;
    case 0x25: cpu->A &= cpu_mem_read(cpu, addr_zero_page(cpu)); update_nz(cpu, cpu->A); break;
    case 0x35: cpu->A &= cpu_mem_read(cpu, addr_zero_page_x(cpu)); update_nz(cpu, cpu->A); break;
    case 0x2D: cpu->A &= cpu_mem_read(cpu, addr_absolute(cpu)); update_nz(cpu, cpu->A); break;
    case 0x3D: addr = addr_absolute_x(cpu, &page_crossed); cpu->A &= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0x39: addr = addr_absolute_y(cpu, &page_crossed); cpu->A &= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0x21: cpu->A &= cpu_mem_read(cpu, addr_indexed_indirect(cpu)); update_nz(cpu, cpu->A); break;
    case 0x31: addr = addr_indirect_indexed(cpu, &page_crossed); cpu->A &= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;

    /* ── ORA ── */
    case 0x09: cpu->A |= cpu_mem_read(cpu, addr_immediate(cpu)); update_nz(cpu, cpu->A); break;
    case 0x05: cpu->A |= cpu_mem_read(cpu, addr_zero_page(cpu)); update_nz(cpu, cpu->A); break;
    case 0x15: cpu->A |= cpu_mem_read(cpu, addr_zero_page_x(cpu)); update_nz(cpu, cpu->A); break;
    case 0x0D: cpu->A |= cpu_mem_read(cpu, addr_absolute(cpu)); update_nz(cpu, cpu->A); break;
    case 0x1D: addr = addr_absolute_x(cpu, &page_crossed); cpu->A |= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0x19: addr = addr_absolute_y(cpu, &page_crossed); cpu->A |= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0x01: cpu->A |= cpu_mem_read(cpu, addr_indexed_indirect(cpu)); update_nz(cpu, cpu->A); break;
    case 0x11: addr = addr_indirect_indexed(cpu, &page_crossed); cpu->A |= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;

    /* ── EOR ── */
    case 0x49: cpu->A ^= cpu_mem_read(cpu, addr_immediate(cpu)); update_nz(cpu, cpu->A); break;
    case 0x45: cpu->A ^= cpu_mem_read(cpu, addr_zero_page(cpu)); update_nz(cpu, cpu->A); break;
    case 0x55: cpu->A ^= cpu_mem_read(cpu, addr_zero_page_x(cpu)); update_nz(cpu, cpu->A); break;
    case 0x4D: cpu->A ^= cpu_mem_read(cpu, addr_absolute(cpu)); update_nz(cpu, cpu->A); break;
    case 0x5D: addr = addr_absolute_x(cpu, &page_crossed); cpu->A ^= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0x59: addr = addr_absolute_y(cpu, &page_crossed); cpu->A ^= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;
    case 0x41: cpu->A ^= cpu_mem_read(cpu, addr_indexed_indirect(cpu)); update_nz(cpu, cpu->A); break;
    case 0x51: addr = addr_indirect_indexed(cpu, &page_crossed); cpu->A ^= cpu_mem_read(cpu, addr); update_nz(cpu, cpu->A); if(page_crossed) extra=1; break;

    /* ── CMP ── */
    case 0xC9: op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr_immediate(cpu))); break;
    case 0xC5: op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr_zero_page(cpu))); break;
    case 0xD5: op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr_zero_page_x(cpu))); break;
    case 0xCD: op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr_absolute(cpu))); break;
    case 0xDD: addr = addr_absolute_x(cpu, &page_crossed); op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;
    case 0xD9: addr = addr_absolute_y(cpu, &page_crossed); op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;
    case 0xC1: op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr_indexed_indirect(cpu))); break;
    case 0xD1: addr = addr_indirect_indexed(cpu, &page_crossed); op_cmp(cpu, cpu->A, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;

    /* ── CPX ── */
    case 0xE0: op_cmp(cpu, cpu->X, cpu_mem_read(cpu, addr_immediate(cpu))); break;
    case 0xE4: op_cmp(cpu, cpu->X, cpu_mem_read(cpu, addr_zero_page(cpu))); break;
    case 0xEC: op_cmp(cpu, cpu->X, cpu_mem_read(cpu, addr_absolute(cpu))); break;

    /* ── CPY ── */
    case 0xC0: op_cmp(cpu, cpu->Y, cpu_mem_read(cpu, addr_immediate(cpu))); break;
    case 0xC4: op_cmp(cpu, cpu->Y, cpu_mem_read(cpu, addr_zero_page(cpu))); break;
    case 0xCC: op_cmp(cpu, cpu->Y, cpu_mem_read(cpu, addr_absolute(cpu))); break;

    /* ── BIT ── */
    case 0x24: val = cpu_mem_read(cpu, addr_zero_page(cpu));
        cpu_set_flag(cpu, FLAG_ZERO, (cpu->A & val) == 0);
        cpu_set_flag(cpu, FLAG_OVERFLOW, (val & 0x40) != 0);
        cpu_set_flag(cpu, FLAG_NEGATIVE, (val & 0x80) != 0);
        break;
    case 0x2C: val = cpu_mem_read(cpu, addr_absolute(cpu));
        cpu_set_flag(cpu, FLAG_ZERO, (cpu->A & val) == 0);
        cpu_set_flag(cpu, FLAG_OVERFLOW, (val & 0x40) != 0);
        cpu_set_flag(cpu, FLAG_NEGATIVE, (val & 0x80) != 0);
        break;

    /* ── ASL (Accumulator) ── */
    case 0x0A:
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x80) != 0);
        cpu->A <<= 1;
        update_nz(cpu, cpu->A);
        break;
    /* ── ASL (Memory) ── */
    case 0x06: addr = addr_zero_page(cpu); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0); result = val << 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0x16: addr = addr_zero_page_x(cpu); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0); result = val << 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0x0E: addr = addr_absolute(cpu); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0); result = val << 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0x1E: addr = addr_absolute_x(cpu, NULL); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0); result = val << 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;

    /* ── LSR (Accumulator) ── */
    case 0x4A:
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x01) != 0);
        cpu->A >>= 1;
        update_nz(cpu, cpu->A);
        break;
    /* ── LSR (Memory) ── */
    case 0x46: addr = addr_zero_page(cpu); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0); result = val >> 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0x56: addr = addr_zero_page_x(cpu); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0); result = val >> 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0x4E: addr = addr_absolute(cpu); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0); result = val >> 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0x5E: addr = addr_absolute_x(cpu, NULL); val = cpu_mem_read(cpu, addr);
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0); result = val >> 1;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;

    /* ── ROL (Accumulator) ── */
    case 0x2A: {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x80) != 0);
        cpu->A = (cpu->A << 1) | c;
        update_nz(cpu, cpu->A);
        break;
    }
    /* ── ROL (Memory) ── */
    case 0x26: addr = addr_zero_page(cpu); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
        result = (val << 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;
    case 0x36: addr = addr_zero_page_x(cpu); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
        result = (val << 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;
    case 0x2E: addr = addr_absolute(cpu); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
        result = (val << 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;
    case 0x3E: addr = addr_absolute_x(cpu, NULL); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 1 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
        result = (val << 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;

    /* ── ROR (Accumulator) ── */
    case 0x6A: {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x01) != 0);
        cpu->A = (cpu->A >> 1) | c;
        update_nz(cpu, cpu->A);
        break;
    }
    /* ── ROR (Memory) ── */
    case 0x66: addr = addr_zero_page(cpu); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
        result = (val >> 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;
    case 0x76: addr = addr_zero_page_x(cpu); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
        result = (val >> 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;
    case 0x6E: addr = addr_absolute(cpu); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
        result = (val >> 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;
    case 0x7E: addr = addr_absolute_x(cpu, NULL); val = cpu_mem_read(cpu, addr); {
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
        cpu_set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
        result = (val >> 1) | c;
        cpu_mem_write(cpu, addr, result); update_nz(cpu, result);
    } break;

    /* ── INC ── */
    case 0xE6: addr = addr_zero_page(cpu); result = cpu_mem_read(cpu, addr) + 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0xF6: addr = addr_zero_page_x(cpu); result = cpu_mem_read(cpu, addr) + 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0xEE: addr = addr_absolute(cpu); result = cpu_mem_read(cpu, addr) + 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0xFE: addr = addr_absolute_x(cpu, NULL); result = cpu_mem_read(cpu, addr) + 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;

    /* ── DEC ── */
    case 0xC6: addr = addr_zero_page(cpu); result = cpu_mem_read(cpu, addr) - 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0xD6: addr = addr_zero_page_x(cpu); result = cpu_mem_read(cpu, addr) - 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0xCE: addr = addr_absolute(cpu); result = cpu_mem_read(cpu, addr) - 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;
    case 0xDE: addr = addr_absolute_x(cpu, NULL); result = cpu_mem_read(cpu, addr) - 1; cpu_mem_write(cpu, addr, result); update_nz(cpu, result); break;

    /* ── INX, INY, DEX, DEY ── */
    case 0xE8: cpu->X++; update_nz(cpu, cpu->X); break;
    case 0xC8: cpu->Y++; update_nz(cpu, cpu->Y); break;
    case 0xCA: cpu->X--; update_nz(cpu, cpu->X); break;
    case 0x88: cpu->Y--; update_nz(cpu, cpu->Y); break;

    /* ── Transfers ── */
    case 0xAA: cpu->X = cpu->A; update_nz(cpu, cpu->X); break;  /* TAX */
    case 0x8A: cpu->A = cpu->X; update_nz(cpu, cpu->A); break;  /* TXA */
    case 0xA8: cpu->Y = cpu->A; update_nz(cpu, cpu->Y); break;  /* TAY */
    case 0x98: cpu->A = cpu->Y; update_nz(cpu, cpu->A); break;  /* TYA */
    case 0xBA: cpu->X = cpu->SP; update_nz(cpu, cpu->X); break; /* TSX */
    case 0x9A: cpu->SP = cpu->X; break;                          /* TXS */

    /* ── Stack ── */
    case 0x48: cpu_push(cpu, cpu->A); break;                     /* PHA */
    case 0x68: cpu->A = cpu_pull(cpu); update_nz(cpu, cpu->A); break; /* PLA */
    case 0x08: cpu_push(cpu, cpu->P | FLAG_BREAK | FLAG_UNUSED); break; /* PHP */
    case 0x28: cpu->P = (cpu_pull(cpu) & ~FLAG_BREAK) | FLAG_UNUSED; break; /* PLP */

    /* ── Branches ── */
    case 0x10: extra = do_branch(cpu, !cpu_get_flag(cpu, FLAG_NEGATIVE)); break; /* BPL */
    case 0x30: extra = do_branch(cpu, cpu_get_flag(cpu, FLAG_NEGATIVE)); break;  /* BMI */
    case 0x50: extra = do_branch(cpu, !cpu_get_flag(cpu, FLAG_OVERFLOW)); break; /* BVC */
    case 0x70: extra = do_branch(cpu, cpu_get_flag(cpu, FLAG_OVERFLOW)); break;  /* BVS */
    case 0x90: extra = do_branch(cpu, !cpu_get_flag(cpu, FLAG_CARRY)); break;    /* BCC */
    case 0xB0: extra = do_branch(cpu, cpu_get_flag(cpu, FLAG_CARRY)); break;     /* BCS */
    case 0xD0: extra = do_branch(cpu, !cpu_get_flag(cpu, FLAG_ZERO)); break;     /* BNE */
    case 0xF0: extra = do_branch(cpu, cpu_get_flag(cpu, FLAG_ZERO)); break;      /* BEQ */

    /* ── JMP ── */
    case 0x4C: cpu->PC = addr_absolute(cpu); break;
    case 0x6C: cpu->PC = addr_indirect(cpu); break;

    /* ── JSR ── */
    case 0x20:
        addr = addr_absolute(cpu);
        cpu_push_word(cpu, cpu->PC - 1);
        cpu->PC = addr;
        break;

    /* ── RTS ── */
    case 0x60:
        cpu->PC = cpu_pull_word(cpu) + 1;
        break;

    /* ── RTI ── */
    case 0x40:
        cpu->P = (cpu_pull(cpu) & ~FLAG_BREAK) | FLAG_UNUSED;
        cpu->PC = cpu_pull_word(cpu);
        if (cpu->irq_trace_fp) {
            fprintf((FILE*)cpu->irq_trace_fp,
                    "%010llu RTI       PC_return=$%04X P=$%02X SP=$%02X\n",
                    (unsigned long long)cpu->cycles, cpu->PC, cpu->P, cpu->SP);
        }
        break;

    /* ── BRK ── */
    case 0x00:
        cpu->PC++;
        cpu_push_word(cpu, cpu->PC);
        cpu_push(cpu, cpu->P | FLAG_BREAK | FLAG_UNUSED);
        cpu_set_flag(cpu, FLAG_INTERRUPT, true);
        cpu->PC = cpu_mem_read(cpu, 0xFFFE) | ((uint16_t)cpu_mem_read(cpu, 0xFFFF) << 8);
        break;

    /* ── Flag instructions ── */
    case 0x18: cpu_set_flag(cpu, FLAG_CARRY, false); break;     /* CLC */
    case 0x38: cpu_set_flag(cpu, FLAG_CARRY, true); break;      /* SEC */
    case 0x58: cpu_set_flag(cpu, FLAG_INTERRUPT, false); break;  /* CLI */
    case 0x78: cpu_set_flag(cpu, FLAG_INTERRUPT, true); break;   /* SEI */
    case 0xD8: cpu_set_flag(cpu, FLAG_DECIMAL, false); break;    /* CLD */
    case 0xF8: cpu_set_flag(cpu, FLAG_DECIMAL, true); break;     /* SED */
    case 0xB8: cpu_set_flag(cpu, FLAG_OVERFLOW, false); break;   /* CLV */

    /* ── NOP ── */
    case 0xEA: break;

    /* ════════════════════════════════════════════════════════════════
     * Illegal / undocumented opcodes (NMOS 6502) — see helpers above
     * ════════════════════════════════════════════════════════════════ */

    /* ── SLO (ASL+ORA) ── */
    case 0x07: op_slo(cpu, addr_zero_page(cpu)); break;
    case 0x17: op_slo(cpu, addr_zero_page_x(cpu)); break;
    case 0x0F: op_slo(cpu, addr_absolute(cpu)); break;
    case 0x1F: op_slo(cpu, addr_absolute_x(cpu, NULL)); break;
    case 0x1B: op_slo(cpu, addr_absolute_y(cpu, NULL)); break;
    case 0x03: op_slo(cpu, addr_indexed_indirect(cpu)); break;
    case 0x13: op_slo(cpu, addr_indirect_indexed(cpu, NULL)); break;

    /* ── RLA (ROL+AND) ── */
    case 0x27: op_rla(cpu, addr_zero_page(cpu)); break;
    case 0x37: op_rla(cpu, addr_zero_page_x(cpu)); break;
    case 0x2F: op_rla(cpu, addr_absolute(cpu)); break;
    case 0x3F: op_rla(cpu, addr_absolute_x(cpu, NULL)); break;
    case 0x3B: op_rla(cpu, addr_absolute_y(cpu, NULL)); break;
    case 0x23: op_rla(cpu, addr_indexed_indirect(cpu)); break;
    case 0x33: op_rla(cpu, addr_indirect_indexed(cpu, NULL)); break;

    /* ── SRE (LSR+EOR) ── */
    case 0x47: op_sre(cpu, addr_zero_page(cpu)); break;
    case 0x57: op_sre(cpu, addr_zero_page_x(cpu)); break;
    case 0x4F: op_sre(cpu, addr_absolute(cpu)); break;
    case 0x5F: op_sre(cpu, addr_absolute_x(cpu, NULL)); break;
    case 0x5B: op_sre(cpu, addr_absolute_y(cpu, NULL)); break;
    case 0x43: op_sre(cpu, addr_indexed_indirect(cpu)); break;
    case 0x53: op_sre(cpu, addr_indirect_indexed(cpu, NULL)); break;

    /* ── RRA (ROR+ADC) ── */
    case 0x67: op_rra(cpu, addr_zero_page(cpu)); break;
    case 0x77: op_rra(cpu, addr_zero_page_x(cpu)); break;
    case 0x6F: op_rra(cpu, addr_absolute(cpu)); break;
    case 0x7F: op_rra(cpu, addr_absolute_x(cpu, NULL)); break;
    case 0x7B: op_rra(cpu, addr_absolute_y(cpu, NULL)); break;
    case 0x63: op_rra(cpu, addr_indexed_indirect(cpu)); break;
    case 0x73: op_rra(cpu, addr_indirect_indexed(cpu, NULL)); break;

    /* ── DCP (DEC+CMP) ── */
    case 0xC7: op_dcp(cpu, addr_zero_page(cpu)); break;
    case 0xD7: op_dcp(cpu, addr_zero_page_x(cpu)); break;
    case 0xCF: op_dcp(cpu, addr_absolute(cpu)); break;
    case 0xDF: op_dcp(cpu, addr_absolute_x(cpu, NULL)); break;
    case 0xDB: op_dcp(cpu, addr_absolute_y(cpu, NULL)); break;
    case 0xC3: op_dcp(cpu, addr_indexed_indirect(cpu)); break;
    case 0xD3: op_dcp(cpu, addr_indirect_indexed(cpu, NULL)); break;

    /* ── ISC (INC+SBC) ── */
    case 0xE7: op_isc(cpu, addr_zero_page(cpu)); break;
    case 0xF7: op_isc(cpu, addr_zero_page_x(cpu)); break;
    case 0xEF: op_isc(cpu, addr_absolute(cpu)); break;
    case 0xFF: op_isc(cpu, addr_absolute_x(cpu, NULL)); break;
    case 0xFB: op_isc(cpu, addr_absolute_y(cpu, NULL)); break;
    case 0xE3: op_isc(cpu, addr_indexed_indirect(cpu)); break;
    case 0xF3: op_isc(cpu, addr_indirect_indexed(cpu, NULL)); break;

    /* ── LAX (LDA+LDX) ── */
    case 0xA7: op_lax(cpu, cpu_mem_read(cpu, addr_zero_page(cpu))); break;
    case 0xB7: op_lax(cpu, cpu_mem_read(cpu, addr_zero_page_y(cpu))); break;
    case 0xAF: op_lax(cpu, cpu_mem_read(cpu, addr_absolute(cpu))); break;
    case 0xBF: addr = addr_absolute_y(cpu, &page_crossed); op_lax(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;
    case 0xA3: op_lax(cpu, cpu_mem_read(cpu, addr_indexed_indirect(cpu))); break;
    case 0xB3: addr = addr_indirect_indexed(cpu, &page_crossed); op_lax(cpu, cpu_mem_read(cpu, addr)); if(page_crossed) extra=1; break;

    /* ── SAX (store A&X) ── */
    case 0x87: cpu_mem_write(cpu, addr_zero_page(cpu), cpu->A & cpu->X); break;
    case 0x97: cpu_mem_write(cpu, addr_zero_page_y(cpu), cpu->A & cpu->X); break;
    case 0x8F: cpu_mem_write(cpu, addr_absolute(cpu), cpu->A & cpu->X); break;
    case 0x83: cpu_mem_write(cpu, addr_indexed_indirect(cpu), cpu->A & cpu->X); break;

    /* ── ANC (AND #imm, C = bit7 of result) ── */
    case 0x0B:
    case 0x2B:
        cpu->A &= cpu_mem_read(cpu, addr_immediate(cpu));
        update_nz(cpu, cpu->A);
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x80) != 0);
        break;

    /* ── ALR/ASR (AND #imm, then LSR A) ── */
    case 0x4B:
        cpu->A &= cpu_mem_read(cpu, addr_immediate(cpu));
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x01) != 0);
        cpu->A >>= 1;
        update_nz(cpu, cpu->A);
        break;

    /* ── ARR (AND #imm, then ROR A, with quirky C/V flags) ── */
    case 0x6B: {
        cpu->A &= cpu_mem_read(cpu, addr_immediate(cpu));
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
        cpu->A = (uint8_t)((cpu->A >> 1) | c);
        update_nz(cpu, cpu->A);
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x40) != 0);
        cpu_set_flag(cpu, FLAG_OVERFLOW, (((cpu->A >> 6) ^ (cpu->A >> 5)) & 0x01) != 0);
        break;
    }

    /* ── SBX/AXS (X = (A & X) - imm, set C like CMP) ── */
    case 0xCB: {
        uint8_t imm = cpu_mem_read(cpu, addr_immediate(cpu));
        uint8_t tmp = cpu->A & cpu->X;
        cpu_set_flag(cpu, FLAG_CARRY, tmp >= imm);
        cpu->X = (uint8_t)(tmp - imm);
        update_nz(cpu, cpu->X);
        break;
    }

    /* ── ANE/XAA (unstable: A = (A | magic) & X & imm) ── */
    case 0x8B: {
        uint8_t imm = cpu_mem_read(cpu, addr_immediate(cpu));
        cpu->A = (uint8_t)((cpu->A | 0xEE) & cpu->X & imm);
        update_nz(cpu, cpu->A);
        break;
    }

    /* ── LXA/LAX#imm (unstable: A = X = (A | magic) & imm) ── */
    case 0xAB: {
        uint8_t imm = cpu_mem_read(cpu, addr_immediate(cpu));
        cpu->A = (uint8_t)((cpu->A | 0xEE) & imm);
        cpu->X = cpu->A;
        update_nz(cpu, cpu->A);
        break;
    }

    /* ── LAS/LAR (A = X = SP = mem & SP) ── */
    case 0xBB: addr = addr_absolute_y(cpu, &page_crossed); {
        uint8_t v = cpu_mem_read(cpu, addr) & cpu->SP;
        cpu->A = v; cpu->X = v; cpu->SP = v;
        update_nz(cpu, v);
    } if(page_crossed) extra=1; break;

    /* ── SHA/AHX (store A & X & (high(addr)+1)) — unstable ── */
    case 0x9F: addr = addr_absolute_y(cpu, NULL);
        cpu_mem_write(cpu, addr, cpu->A & cpu->X & (uint8_t)((addr >> 8) + 1)); break;
    case 0x93: addr = addr_indirect_indexed(cpu, NULL);
        cpu_mem_write(cpu, addr, cpu->A & cpu->X & (uint8_t)((addr >> 8) + 1)); break;

    /* ── SHX (store X & (high(addr)+1)) — unstable ── */
    case 0x9E: addr = addr_absolute_y(cpu, NULL);
        cpu_mem_write(cpu, addr, cpu->X & (uint8_t)((addr >> 8) + 1)); break;

    /* ── SHY (store Y & (high(addr)+1)) — unstable ── */
    case 0x9C: addr = addr_absolute_x(cpu, NULL);
        cpu_mem_write(cpu, addr, cpu->Y & (uint8_t)((addr >> 8) + 1)); break;

    /* ── TAS/SHS (SP = A & X, store SP & (high(addr)+1)) — unstable ── */
    case 0x9B: addr = addr_absolute_y(cpu, NULL);
        cpu->SP = cpu->A & cpu->X;
        cpu_mem_write(cpu, addr, cpu->SP & (uint8_t)((addr >> 8) + 1)); break;

    /* ── Undocumented multi-byte NOPs: still perform a dummy operand read ── */
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:        /* NOP #imm */
        (void)cpu_mem_read(cpu, addr_immediate(cpu)); break;
    case 0x04: case 0x44: case 0x64:                              /* NOP zp */
        (void)cpu_mem_read(cpu, addr_zero_page(cpu)); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: /* NOP zp,X */
        (void)cpu_mem_read(cpu, addr_zero_page_x(cpu)); break;
    case 0x0C:                                                    /* NOP abs */
        (void)cpu_mem_read(cpu, addr_absolute(cpu)); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: /* NOP abs,X */
        addr = addr_absolute_x(cpu, &page_crossed);
        (void)cpu_mem_read(cpu, addr);
        if (page_crossed) extra = 1;
        break;
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: /* NOP (implied) */
        break;

    /* ── JAM/KIL: freeze the CPU (real NMOS halts the bus) ── */
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        cpu->halted = true;
        break;

    /* ── Safety net: any opcode not explicitly handled above ── */
    default: {
        uint8_t sz = opcode_table[opcode].size;
        for (uint8_t i = 1; i < sz; i++) {
            cpu_fetch_byte(cpu);
        }
        break;
    }
    }

    return cycles + extra;
}
