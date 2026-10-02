/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file debugger_asm.c
 * @brief Débogueur : assembleur en ligne
 * @author bmarty <bmarty@mailo.com>
 *
 * Découpé de src/debugger.c (sprint F du plan d'architecture), sans changement
 * de comportement ; symboles partagés : include/debugger_internal.h.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "debugger.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "cpu/cpu_internal.h"   /* opcode_table[] — source of truth for the assembler */
#include "memory/memory.h"
#include "io/via6522.h"
#include "audio/audio.h"
#include "savestate.h"
#include "debugger_internal.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  INLINE ASSEMBLER (a addr MNEMONIC [operand])                       */
/* ═══════════════════════════════════════════════════════════════════ */

/* First opcode byte whose mnemonic and addressing mode match (the legal
 * 6502 set has at most one entry per mnemonic+mode). -1 if none. */
static int asm_find_opcode(const char* mnem, addressing_mode_t mode) {
    for (int i = 0; i < 256; i++) {
        if (opcode_table[i].mode == mode &&
            strcasecmp(opcode_table[i].name, mnem) == 0) {
            return i;
        }
    }
    return -1;
}

static bool asm_has_mode(const char* mnem, addressing_mode_t mode) {
    return asm_find_opcode(mnem, mode) >= 0;
}

/* Parse a numeric/symbol operand value. Hex by default ($ optional), symbols
 * resolved via the loaded table. *is_word is set when the literal clearly
 * denotes 16 bits (>2 hex digits) or the value exceeds a byte — used to pick
 * zero-page vs absolute. Returns false if unparseable. */
static bool asm_parse_value(emulator_t* emu, const char* s,
                            uint16_t* val, bool* is_word) {
    *is_word = false;
    if (!s || !*s) return false;
    uint16_t sv;
    if (symbol_resolve(&emu->symbols, s, &sv)) {
        *val = sv;
        *is_word = (sv > 0xFF);
        return true;
    }
    const char* p = (*s == '$') ? s + 1 : s;
    int digits = 0;
    for (const char* q = p; *q; q++) {
        if (!isxdigit((unsigned char)*q)) return false;
        digits++;
    }
    if (digits == 0) return false;
    char* end = NULL;
    unsigned long v = strtoul(p, &end, 16);
    if (*end || v > 0xFFFF) return false;
    *val = (uint16_t)v;
    *is_word = (digits > 2) || (v > 0xFF);
    return true;
}

/* Assemble "MNEM [operand]" at `addr`, writing the bytes to memory.
 * Returns the instruction length (1-3) or 0 on error (message printed). */
int dbg_assemble_one(emulator_t* emu, uint16_t addr,
                        const char* mnem_in, const char* operand_in) {
    char mnem[8];
    size_t mi = 0;
    for (const char* p = mnem_in; *p && mi < sizeof(mnem) - 1; p++)
        mnem[mi++] = (char)toupper((unsigned char)*p);
    mnem[mi] = '\0';
    if (mi == 0) { printf("  Empty mnemonic\n"); return 0; }

    /* Operand: strip whitespace, upper-case (hex digits + register letters). */
    char op[40];
    size_t oi = 0;
    for (const char* p = operand_in ? operand_in : ""; *p; p++) {
        if (!isspace((unsigned char)*p) && oi < sizeof(op) - 1)
            op[oi++] = (char)toupper((unsigned char)*p);
    }
    op[oi] = '\0';

    addressing_mode_t mode;
    uint16_t value = 0;
    bool is_word = false;

    if (op[0] == '\0' || (op[0] == 'A' && op[1] == '\0')) {
        /* No operand → implicit; bare "A" → accumulator. */
        if (op[0] == '\0' && asm_has_mode(mnem, ADDR_IMPLICIT))
            mode = ADDR_IMPLICIT;
        else if (asm_has_mode(mnem, ADDR_ACCUMULATOR))
            mode = ADDR_ACCUMULATOR;
        else if (asm_has_mode(mnem, ADDR_IMPLICIT))
            mode = ADDR_IMPLICIT;
        else { printf("  %s: no implicit/accumulator form\n", mnem); return 0; }
    } else if (op[0] == '#') {
        mode = ADDR_IMMEDIATE;
        if (!asm_parse_value(emu, op + 1, &value, &is_word) || value > 0xFF) {
            printf("  Bad immediate operand: #%s\n", op + 1); return 0;
        }
    } else if (op[0] == '(') {
        /* Indirect forms. */
        char inner[40];
        const char* tail;
        if (oi >= 4 && strcmp(op + oi - 3, ",X)") == 0) {
            size_t n = oi - 4;                 /* between '(' and ',X)' */
            memcpy(inner, op + 1, n); inner[n] = '\0';
            mode = ADDR_INDEXED_INDIRECT; tail = ",X)";
        } else if (oi >= 4 && strcmp(op + oi - 3, "),Y") == 0) {
            size_t n = oi - 4;                 /* between '(' and '),Y' */
            memcpy(inner, op + 1, n); inner[n] = '\0';
            mode = ADDR_INDIRECT_INDEXED; tail = "),Y";
        } else if (op[oi - 1] == ')') {
            size_t n = oi - 2;                 /* between '(' and ')' */
            memcpy(inner, op + 1, n); inner[n] = '\0';
            mode = ADDR_INDIRECT; tail = ")";
        } else { printf("  Malformed indirect operand: %s\n", op); return 0; }
        (void)tail;
        if (!asm_parse_value(emu, inner, &value, &is_word)) {
            printf("  Bad operand value: %s\n", inner); return 0;
        }
    } else {
        /* Plain / indexed. */
        char base[40];
        strncpy(base, op, sizeof(base) - 1); base[sizeof(base) - 1] = '\0';
        int idx = 0;  /* 0=none, 1=X, 2=Y */
        if (oi >= 2 && strcmp(op + oi - 2, ",X") == 0) { base[oi - 2] = '\0'; idx = 1; }
        else if (oi >= 2 && strcmp(op + oi - 2, ",Y") == 0) { base[oi - 2] = '\0'; idx = 2; }

        if (!asm_parse_value(emu, base, &value, &is_word)) {
            printf("  Bad operand value: %s\n", base); return 0;
        }
        bool zp = (value <= 0xFF) && !is_word;
        if (idx == 1) {
            mode = (zp && asm_has_mode(mnem, ADDR_ZERO_PAGE_X))
                       ? ADDR_ZERO_PAGE_X : ADDR_ABSOLUTE_X;
        } else if (idx == 2) {
            mode = (zp && asm_has_mode(mnem, ADDR_ZERO_PAGE_Y))
                       ? ADDR_ZERO_PAGE_Y : ADDR_ABSOLUTE_Y;
        } else if (asm_has_mode(mnem, ADDR_RELATIVE)) {
            /* Branch: operand is the absolute target. */
            int off = (int)value - (int)(addr + 2);
            if (off < -128 || off > 127) {
                printf("  Branch out of range (%+d) to $%04X\n", off, value);
                return 0;
            }
            int rb = asm_find_opcode(mnem, ADDR_RELATIVE);
            memory_write(&emu->memory, addr, (uint8_t)rb);
            memory_write(&emu->memory, (uint16_t)(addr + 1), (uint8_t)(off & 0xFF));
            return 2;
        } else {
            mode = (zp && asm_has_mode(mnem, ADDR_ZERO_PAGE))
                       ? ADDR_ZERO_PAGE : ADDR_ABSOLUTE;
        }
    }

    int ob = asm_find_opcode(mnem, mode);
    if (ob < 0) {
        printf("  %s: invalid addressing mode for this mnemonic\n", mnem);
        return 0;
    }
    int size = opcode_table[ob].size;
    memory_write(&emu->memory, addr, (uint8_t)ob);
    if (size >= 2)
        memory_write(&emu->memory, (uint16_t)(addr + 1), (uint8_t)(value & 0xFF));
    if (size >= 3)
        memory_write(&emu->memory, (uint16_t)(addr + 2), (uint8_t)(value >> 8));
    return size;
}

