/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file debugger_mem.c
 * @brief Debugger: memory search, banks, hunt, region save/load
 * @author bmarty <bmarty@mailo.com>
 *
 * Split out of src/debugger.c (sprint F of the architecture plan), with no
 * behaviour change; shared symbols: include/debugger_internal.h.
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
/*  MEMORY SEARCH (find)                                               */
/* ═══════════════════════════════════════════════════════════════════ */

/* Side-effect-free memory peek: read the backing RAM array for $0000-$BFFF
 * (so searching never touches VIA/ACIA I/O registers and clears flags), and
 * the side-effect-free CPU view (ROM/overlay) for $C000-$FFFF. */
uint8_t dbg_peek(emulator_t* emu, uint16_t a) {
    if (a < RAM_SIZE) return emu->memory.ram[a];
    return memory_peek(&emu->memory, a);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  BANK-AWARE INSPECTION (Epic 6 / US 2)                              */
/* ═══════════════════════════════════════════════════════════════════ */

uint8_t debugger_peek_bank(emulator_t* emu, uint16_t addr, peek_bank_t bank) {
    memory_t* m = &emu->memory;
    switch (bank) {
        case PEEK_RAM:
            /* Underlying RAM, including the RAM hidden behind the ROM overlay. */
            if (addr < 0xC000) return m->ram[addr];
            return m->upper_ram[addr - 0xC000];
        case PEEK_ROM:
            /* BASIC/monitor ROM only exists at $C000-$FFFF. */
            if (addr < 0xC000) return 0x00;
            return m->rom[addr - 0xC000];
        case PEEK_OVERLAY:
            /* Microdisc overlay ROM is mapped at $E000-$FFFF. */
            if (addr < 0xE000) return 0x00;
            {
                uint16_t off = (uint16_t)(addr - 0xE000);
                if (m->overlay_rom && off < m->overlay_rom_size)
                    return m->overlay_rom[off];
                return 0xFF;
            }
        case PEEK_CPU:
        default:
            return dbg_peek(emu, addr);
    }
}

bool debugger_parse_bank(const char* s, peek_bank_t* out) {
    if (!s || !*s) return false;
    if      (strcasecmp(s, "cpu") == 0)     *out = PEEK_CPU;
    else if (strcasecmp(s, "ram") == 0)     *out = PEEK_RAM;
    else if (strcasecmp(s, "rom") == 0)     *out = PEEK_ROM;
    else if (strcasecmp(s, "overlay") == 0 ||
             strcasecmp(s, "disk") == 0)    *out = PEEK_OVERLAY;
    else return false;
    return true;
}

const char* debugger_bank_name(peek_bank_t bank) {
    switch (bank) {
        case PEEK_RAM:     return "ram";
        case PEEK_ROM:     return "rom";
        case PEEK_OVERLAY: return "overlay";
        case PEEK_CPU:
        default:           return "cpu";
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ITERATIVE MEMORY SEARCH — cheat-finder (hunt)                      */
/* ═══════════════════════════════════════════════════════════════════ */

/* Candidate set + previous-values snapshot over the whole address space.
 * File-static (128 KB) so debugger_t stays small, mirroring the undo ring. */
bool     dbg_hunt_cand[0x10000];
static uint8_t  hunt_prev[0x10000];
bool     dbg_hunt_active = false;
uint32_t dbg_hunt_count  = 0;

static void hunt_snapshot(emulator_t* emu) {
    for (uint32_t a = 0; a < 0x10000; a++)
        hunt_prev[a] = dbg_peek(emu, (uint16_t)a);
}

/* Begin a hunt: every address becomes a candidate. */
void debugger_hunt_start(emulator_t* emu) {
    for (uint32_t a = 0; a < 0x10000; a++) dbg_hunt_cand[a] = true;
    dbg_hunt_count = 0x10000;
    hunt_snapshot(emu);
    dbg_hunt_active = true;
}

uint32_t debugger_hunt_count(void) { return dbg_hunt_count; }
bool     debugger_hunt_active(void) { return dbg_hunt_active; }
void     debugger_hunt_clear(void) { dbg_hunt_active = false; dbg_hunt_count = 0; }

uint32_t debugger_hunt_list(emulator_t* emu, uint16_t* out, uint32_t max, uint8_t* out_vals) {
    uint32_t n = 0;
    for (uint32_t a = 0; a < 0x10000 && n < max; a++) {
        if (!dbg_hunt_cand[a]) continue;
        out[n] = (uint16_t)a;
        if (out_vals) out_vals[n] = dbg_peek(emu, (uint16_t)a);
        n++;
    }
    return n;
}

/* Narrow the candidate set with a predicate, then re-snapshot for the next
 * relative comparison. Returns the surviving candidate count. */
uint32_t debugger_hunt_refine(emulator_t* emu, hunt_pred_t pred, uint8_t val) {
    uint32_t kept = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (!dbg_hunt_cand[a]) continue;
        uint8_t cur = dbg_peek(emu, (uint16_t)a);
        bool keep = false;
        switch (pred) {
            case HUNT_EQ:        keep = (cur == val);          break;
            case HUNT_UNCHANGED: keep = (cur == hunt_prev[a]); break;
            case HUNT_CHANGED:   keep = (cur != hunt_prev[a]); break;
            case HUNT_GT:        keep = (cur >  hunt_prev[a]); break;
            case HUNT_LT:        keep = (cur <  hunt_prev[a]); break;
        }
        if (keep) kept++; else dbg_hunt_cand[a] = false;
    }
    dbg_hunt_count = kept;
    hunt_snapshot(emu);
    return kept;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MEMORY ⇄ FILE (save / load region)                                 */
/* ═══════════════════════════════════════════════════════════════════ */

bool debugger_save_region(emulator_t* emu, const char* path,
                          uint16_t addr, uint32_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    for (uint32_t i = 0; i < len; i++) {
        if (fputc(dbg_peek(emu, (uint16_t)(addr + i)), f) == EOF) {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

long debugger_load_region(emulator_t* emu, const char* path, uint16_t addr) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = 0;
    int c;
    while ((c = fgetc(f)) != EOF && (addr + n) < 0x10000) {
        memory_write(&emu->memory, (uint16_t)(addr + n), (uint8_t)c);
        n++;
    }
    fclose(f);
    return n;
}

