/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file debugger.c
 * @brief Interactive debugger for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.1.0-alpha
 *
 * REPL debugger with breakpoints, watchpoints, single-step,
 * memory dump, disassembly, register inspection, and more.
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

/* Save-state is an optional link dependency: the `ss`/`sl` REPL commands use it,
 * but unit-test binaries that link debugger.c standalone must not drag in the
 * whole emulator serializer.
 *
 * A weak *undefined reference* resolving to NULL works on ELF (Linux) but not
 * on Mach-O (macOS) static links. The portable idiom is a weak *definition*:
 * these stubs are used by standalone test binaries and are overridden by the
 * strong symbols in savestate.c when the full emulator is linked. Works
 * identically under GNU ld and macOS ld. */
__attribute__((weak)) bool savestate_save(const emulator_t* emu, const char* filename) {
    (void)emu; (void)filename;
    return false;   /* serializer not linked in this binary */
}
__attribute__((weak)) bool savestate_load(emulator_t* emu, const char* filename) {
    (void)emu; (void)filename;
    return false;
}

/* Parse an address argument: tries the symbol table first (case-insensitive),
 * then falls back to numeric parsing. Hex is the default base; `$`/`0x` force
 * hex and `%` forces binary. Returns true if recognised. */
bool dbg_parse_addr(const emulator_t* emu, const char* s, uint16_t* out) {
    if (!s || !*s) return false;
    if (symbol_resolve(&emu->symbols, s, out)) return true;
    int base = 16;
    if (*s == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    else if (*s == '%') { s++; base = 2; }
    char* end = NULL;
    unsigned long v = strtoul(s, &end, base);
    if (end == s || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INIT                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

void debugger_init(debugger_t* dbg) {
    memset(dbg, 0, sizeof(*dbg));
    dbg->active = false;
    dbg->step_mode = false;
    dbg->num_breakpoints = 0;
    dbg->num_watchpoints = 0;
    dbg->watch_triggered = false;
    dbg->has_temp_breakpoint = false;
    dbg->last_raster_line = -1;
    for (int i = 0; i < 8; i++) dbg->raster_bps[i] = -1;
    dbg->undo_head = 0;
    dbg->undo_count = 0;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  UNDO RING (sprint 34d5 P2-F)                                       */
/* ═══════════════════════════════════════════════════════════════════ */
#define UNDO_RING_DEPTH 16

typedef struct undo_snapshot_s {
    /* CPU subset (data only — no pointers). */
    uint8_t  A, X, Y, SP, P;
    uint16_t PC;
    uint64_t cycles;
    uint32_t cycles_left;
    bool     halted;
    bool     nmi_pending;
    uint8_t  irq;
    /* Memory subset (data only — no callbacks). */
    uint8_t  ram[RAM_SIZE];           /* 64 KB */
    uint8_t  upper_ram[ROM_SIZE];     /* 16 KB */
    memory_bank_t charset_bank;
    bool     rom_enabled;
    bool     overlay_active;
    bool     basic_rom_disabled;
    /* Frame position so raster bps stay coherent post-rewind. */
    int      frame_cycles;
} undo_snapshot_t;

/* File-static ring (~1.3 MB) — kept out of debugger_t so emulator_t stays
 * stack-friendly. Single-threaded REPL: no concurrency. */
static undo_snapshot_t g_undo_ring[UNDO_RING_DEPTH];

void dbg_undo_push(debugger_t* dbg, emulator_t* emu) {
    undo_snapshot_t* s = &g_undo_ring[dbg->undo_head];
    s->A = emu->cpu.A;     s->X = emu->cpu.X;     s->Y = emu->cpu.Y;
    s->SP = emu->cpu.SP;   s->P = emu->cpu.P;     s->PC = emu->cpu.PC;
    s->cycles = emu->cpu.cycles;
    s->cycles_left = emu->cpu.cycles_left;
    s->halted = emu->cpu.halted;
    s->nmi_pending = emu->cpu.nmi_pending;
    s->irq = emu->cpu.irq;
    memcpy(s->ram,       emu->memory.ram,       RAM_SIZE);
    memcpy(s->upper_ram, emu->memory.upper_ram, ROM_SIZE);
    s->charset_bank      = emu->memory.charset_bank;
    s->rom_enabled       = emu->memory.rom_enabled;
    s->overlay_active    = emu->memory.overlay_active;
    s->basic_rom_disabled = emu->memory.basic_rom_disabled;
    s->frame_cycles      = emu->frame_cycles;
    dbg->undo_head = (uint8_t)((dbg->undo_head + 1) % UNDO_RING_DEPTH);
    if (dbg->undo_count < UNDO_RING_DEPTH) dbg->undo_count++;
}

bool dbg_undo_pop(debugger_t* dbg, emulator_t* emu) {
    if (dbg->undo_count == 0) return false;
    dbg->undo_head = (uint8_t)((dbg->undo_head + UNDO_RING_DEPTH - 1)
                               % UNDO_RING_DEPTH);
    dbg->undo_count--;
    const undo_snapshot_t* s = &g_undo_ring[dbg->undo_head];
    emu->cpu.A = s->A;       emu->cpu.X = s->X;     emu->cpu.Y = s->Y;
    emu->cpu.SP = s->SP;     emu->cpu.P = s->P;     emu->cpu.PC = s->PC;
    emu->cpu.cycles = s->cycles;
    emu->cpu.cycles_left = s->cycles_left;
    emu->cpu.halted = s->halted;
    emu->cpu.nmi_pending = s->nmi_pending;
    emu->cpu.irq = s->irq;
    memcpy(emu->memory.ram,       s->ram,       RAM_SIZE);
    memcpy(emu->memory.upper_ram, s->upper_ram, ROM_SIZE);
    emu->memory.charset_bank       = s->charset_bank;
    emu->memory.rom_enabled        = s->rom_enabled;
    emu->memory.overlay_active     = s->overlay_active;
    emu->memory.basic_rom_disabled = s->basic_rom_disabled;
    emu->frame_cycles              = s->frame_cycles;
    /* Reset raster-bp transition tracker — restored frame_cycles is the
     * authoritative observation now. */
    dbg->last_raster_line = -1;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  BREAKPOINT MANAGEMENT                                              */
/* ═══════════════════════════════════════════════════════════════════ */

/* Parse "REG OP VALUE" or "M[ADDR] OP VALUE".
 * Returns true on success and fills out_cond. Whitespace is permissive.
 * VALUE: hex ($XX or 0xXX), decimal, or symbol name from emu->symbols. */
const char* dbg_skip_ws(const char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Advance past the current whitespace-delimited token. */
const char* dbg_skip_token(const char* s) {
    while (*s && !isspace((unsigned char)*s)) s++;
    return s;
}

/* Parse a numeric literal (no symbol lookup): `$`/`0x` hex, `%` binary,
 * else decimal. Returns false if nothing consumed. */
static bool parse_num_literal(const char* s, uint32_t* out) {
    int base = 10;
    if (*s == '$') { s++; base = 16; }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (*s == '%') { s++; base = 2; }
    char* end = NULL;
    unsigned long v = strtoul(s, &end, base);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

/* Parse a single "REG OP VALUE" or "M[ADDR] OP VALUE" comparison. On success
 * fills *out and sets *endp just past the consumed VALUE token. The VALUE token
 * ends at whitespace or a '&'/'|' connector so compound expressions parse with
 * or without surrounding spaces. */
static bool parse_one_cond(const emulator_t* emu, const char* text,
                           bp_condition_t* out, const char** endp) {
    memset(out, 0, sizeof(*out));
    const char* p = dbg_skip_ws(text);

    /* Operand: A, X, Y, SP, P, PC, or M[ADDR] */
    if (p[0] == 'M' && p[1] == '[') {
        const char* a = p + 2;
        char addr_buf[48];
        size_t n = 0;
        while (a[n] && a[n] != ']' && n < sizeof(addr_buf) - 1) {
            addr_buf[n] = a[n]; n++;
        }
        if (a[n] != ']') return false;
        addr_buf[n] = '\0';
        uint16_t addr;
        if (!dbg_parse_addr(emu, addr_buf, &addr)) return false;
        out->operand = BP_OPERAND_MEM;
        out->mem_addr = addr;
        p = a + n + 1;
    } else if (p[0] == 'P' && p[1] == 'C' && !isalnum((unsigned char)p[2])) {
        out->operand = BP_OPERAND_PC; p += 2;
    } else if (p[0] == 'S' && p[1] == 'P' && !isalnum((unsigned char)p[2])) {
        out->operand = BP_OPERAND_SP; p += 2;
    } else if ((p[0] == 'A' || p[0] == 'X' || p[0] == 'Y' || p[0] == 'P')
               && !isalnum((unsigned char)p[1])) {
        switch (p[0]) {
            case 'A': out->operand = BP_OPERAND_A; break;
            case 'X': out->operand = BP_OPERAND_X; break;
            case 'Y': out->operand = BP_OPERAND_Y; break;
            case 'P': out->operand = BP_OPERAND_P; break;
        }
        p++;
    } else {
        return false;
    }

    p = dbg_skip_ws(p);

    /* Operator */
    if (p[0] == '=' && p[1] == '=') { out->op = BP_OP_EQ; p += 2; }
    else if (p[0] == '!' && p[1] == '=') { out->op = BP_OP_NE; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { out->op = BP_OP_LE; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { out->op = BP_OP_GE; p += 2; }
    else if (p[0] == '<') { out->op = BP_OP_LT; p++; }
    else if (p[0] == '>') { out->op = BP_OP_GT; p++; }
    else return false;

    p = dbg_skip_ws(p);

    /* RHS value: token ends at whitespace or a connector (& / |) */
    char rhs_buf[48];
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) &&
           p[n] != '&' && p[n] != '|' && n < sizeof(rhs_buf) - 1) {
        rhs_buf[n] = p[n]; n++;
    }
    rhs_buf[n] = '\0';
    if (n == 0) return false;
    uint16_t v16;
    if (symbol_resolve(&emu->symbols, rhs_buf, &v16)) {
        out->value = v16;
    } else {
        /* Bare hex digits (no prefix) are still accepted as hex, matching the
         * historical single-comparison behaviour. */
        const char* s = rhs_buf;
        if (*s != '$' && *s != '%' &&
            !(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) &&
            strpbrk(s, "ABCDEFabcdef")) {
            char* end = NULL;
            unsigned long v = strtoul(s, &end, 16);
            if (end == s) return false;
            out->value = (uint32_t)v;
        } else if (!parse_num_literal(rhs_buf, &out->value)) {
            return false;
        }
    }
    if (endp) *endp = p + n;
    return true;
}

/* Parse a compound condition: one or more comparisons joined by `&&`/`||`
 * (up to BP_MAX_TERMS), evaluated left-to-right. */
bool dbg_parse_condexpr(const emulator_t* emu, const char* text,
                           bp_condexpr_t* out) {
    memset(out, 0, sizeof(*out));
    const char* p = text;
    for (;;) {
        if (out->num_terms >= BP_MAX_TERMS) return false;
        const char* end = NULL;
        if (!parse_one_cond(emu, p, &out->terms[out->num_terms], &end))
            return false;
        out->num_terms++;
        p = dbg_skip_ws(end);
        if (!*p) break;
        if (p[0] == '&' && p[1] == '&') {
            if (out->num_terms >= BP_MAX_TERMS) return false;
            out->conn[out->num_terms - 1] = BP_CONN_AND; p += 2;
        } else if (p[0] == '|' && p[1] == '|') {
            if (out->num_terms >= BP_MAX_TERMS) return false;
            out->conn[out->num_terms - 1] = BP_CONN_OR; p += 2;
        } else {
            return false;
        }
        p = dbg_skip_ws(p);
    }
    return out->num_terms > 0;
}

int debugger_add_breakpoint(debugger_t* dbg, uint16_t addr) {
    if (dbg->num_breakpoints >= DEBUGGER_MAX_BREAKPOINTS)
        return -1;
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        if (dbg->breakpoints[i].addr == addr && !dbg->breakpoints[i].has_cond)
            return i;
    }
    breakpoint_t* bp = &dbg->breakpoints[dbg->num_breakpoints];
    memset(bp, 0, sizeof(*bp));
    bp->addr = addr;
    return dbg->num_breakpoints++;
}

bool debugger_remove_breakpoint(debugger_t* dbg, int index) {
    if (index < 0 || index >= dbg->num_breakpoints)
        return false;
    for (int i = index; i < dbg->num_breakpoints - 1; i++) {
        dbg->breakpoints[i] = dbg->breakpoints[i + 1];
    }
    dbg->num_breakpoints--;
    return true;
}

int debugger_add_cond_breakpoint(debugger_t* dbg, const emulator_t* emu,
                                 uint16_t addr, const char* expr) {
    bp_condexpr_t cond;
    if (!dbg_parse_condexpr(emu, expr, &cond)) return -2;   /* unparseable */
    int idx = debugger_add_breakpoint(dbg, addr);
    if (idx < 0) return -1;                              /* table full */
    breakpoint_t* bp = &dbg->breakpoints[idx];
    bp->has_cond = true;
    bp->cond = cond;
    strncpy(bp->cond_text, expr, sizeof(bp->cond_text) - 1);
    bp->cond_text[sizeof(bp->cond_text) - 1] = '\0';
    size_t cl = strlen(bp->cond_text);
    while (cl > 0 && isspace((unsigned char)bp->cond_text[cl - 1]))
        bp->cond_text[--cl] = '\0';
    return idx;
}

/* Evaluate a condition against current emulator state. */
static bool eval_cond(const bp_condition_t* c, const emulator_t* emu) {
    uint32_t lhs = 0;
    switch (c->operand) {
        case BP_OPERAND_A:  lhs = emu->cpu.A;  break;
        case BP_OPERAND_X:  lhs = emu->cpu.X;  break;
        case BP_OPERAND_Y:  lhs = emu->cpu.Y;  break;
        case BP_OPERAND_SP: lhs = emu->cpu.SP; break;
        case BP_OPERAND_P:  lhs = emu->cpu.P;  break;
        case BP_OPERAND_PC: lhs = emu->cpu.PC; break;
        case BP_OPERAND_MEM:
            lhs = memory_peek((memory_t*)&emu->memory, c->mem_addr);
            break;
        default: return false;
    }
    uint32_t rhs = c->value;
    switch (c->op) {
        case BP_OP_EQ: return lhs == rhs;
        case BP_OP_NE: return lhs != rhs;
        case BP_OP_LT: return lhs <  rhs;
        case BP_OP_LE: return lhs <= rhs;
        case BP_OP_GT: return lhs >  rhs;
        case BP_OP_GE: return lhs >= rhs;
    }
    return false;
}

/* Evaluate a compound condition, folding terms strictly left-to-right. */
static bool eval_condexpr(const bp_condexpr_t* e, const emulator_t* emu) {
    if (e->num_terms == 0) return true;
    bool result = eval_cond(&e->terms[0], emu);
    for (int i = 1; i < e->num_terms; i++) {
        bool t = eval_cond(&e->terms[i], emu);
        if (e->conn[i - 1] == BP_CONN_AND) result = result && t;
        else                                result = result || t;
    }
    return result;
}

bool debugger_is_breakpoint(const debugger_t* dbg, uint16_t pc) {
    /* Without emulator state we can't evaluate conditions — caller (cpu_step
     * path) uses debugger_check_pc() instead for full evaluation. This stays
     * for legacy callers and simply matches on address (acts as a fast pre-filter). */
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        if (dbg->breakpoints[i].addr == pc)
            return true;
    }
    return false;
}

/* Full PC-breakpoint check including condition evaluation. */
static bool debugger_check_pc(const debugger_t* dbg, const emulator_t* emu) {
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        if (dbg->breakpoints[i].addr != emu->cpu.PC) continue;
        if (!dbg->breakpoints[i].has_cond) return true;
        if (eval_condexpr(&dbg->breakpoints[i].cond, emu)) return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  WATCHPOINT MANAGEMENT                                              */
/* ═══════════════════════════════════════════════════════════════════ */

const char* dbg_watch_mode_name(watch_mode_t m) {
    switch (m) {
        case WATCH_WRITE:  return "write";
        case WATCH_READ:   return "read";
        case WATCH_ACCESS: return "access";
        case WATCH_CHANGE: return "change";
    }
    return "?";
}

int debugger_add_watchpoint_mode(debugger_t* dbg, uint16_t addr, watch_mode_t mode) {
    if (dbg->num_watchpoints >= DEBUGGER_MAX_WATCHPOINTS)
        return -1;
    /* Check for duplicate (same address AND mode) */
    for (int i = 0; i < dbg->num_watchpoints; i++) {
        if (dbg->watchpoints[i].addr == addr && dbg->watchpoints[i].mode == mode)
            return i;
    }
    watchpoint_t* w = &dbg->watchpoints[dbg->num_watchpoints];
    w->addr = addr;
    w->mode = mode;
    w->last_value = 0;
    w->has_last = false;
    return dbg->num_watchpoints++;
}

int debugger_add_watchpoint(debugger_t* dbg, uint16_t addr) {
    return debugger_add_watchpoint_mode(dbg, addr, WATCH_WRITE);
}

bool debugger_remove_watchpoint(debugger_t* dbg, int index) {
    if (index < 0 || index >= dbg->num_watchpoints)
        return false;
    for (int i = index; i < dbg->num_watchpoints - 1; i++) {
        dbg->watchpoints[i] = dbg->watchpoints[i + 1];
    }
    dbg->num_watchpoints--;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ACCESS-FLAG MAP (Epic 6 / US 3) — per-byte r/w/x breakpoints        */
/* ═══════════════════════════════════════════════════════════════════ */

/* One flag byte (AMAP_R|AMAP_W|AMAP_X) per address. File-static (64 KB) so
 * debugger_t stays small, like the hunt/undo buffers. */
static uint8_t  g_amap[0x10000];
static uint32_t g_amap_count = 0;   /* bytes with any flag set */
static bool     g_amap_rw = false;  /* any R or W flag present → needs mem trace */

static void amap_recount(void) {
    g_amap_count = 0;
    g_amap_rw = false;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_amap[a]) {
            g_amap_count++;
            if (g_amap[a] & (AMAP_R | AMAP_W)) g_amap_rw = true;
        }
    }
}

uint32_t debugger_amap_set(uint16_t start, uint16_t end, uint8_t flags) {
    if (end < start) { uint16_t t = start; start = end; end = t; }
    flags &= (AMAP_R | AMAP_W | AMAP_X);
    for (uint32_t a = start; a <= end; a++) g_amap[a] |= flags;
    amap_recount();
    return (uint32_t)(end - start) + 1;
}

void     debugger_amap_clear(void) { memset(g_amap, 0, sizeof(g_amap)); g_amap_count = 0; g_amap_rw = false; }
uint8_t  debugger_amap_get(uint16_t addr) { return g_amap[addr]; }
bool     debugger_amap_active(void) { return g_amap_rw; }
uint32_t debugger_amap_count(void) { return g_amap_count; }

bool debugger_amap_parse_flags(const char* s, uint8_t* out) {
    if (!s || !*s) return false;
    uint8_t f = 0;
    for (; *s; s++) {
        switch (*s) {
            case 'r': case 'R': f |= AMAP_R; break;
            case 'w': case 'W': f |= AMAP_W; break;
            case 'x': case 'X': f |= AMAP_X; break;
            default: return false;
        }
    }
    *out = f;
    return f != 0;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MEMORY TRACE CALLBACK (for watchpoints)                            */
/* ═══════════════════════════════════════════════════════════════════ */

/* Global pointer to active debugger (needed by trace callback) */
static debugger_t* g_trace_debugger = NULL;

static void watchpoint_trace_callback(uint16_t address, uint8_t value, mem_access_type_t type) {
    debugger_t* d = g_trace_debugger;
    if (!d) return;
    /* Access-flag map (US 3): a read/write to a flagged byte breaks. */
    if (g_amap_rw) {
        uint8_t f = g_amap[address];
        if ((type == MEM_READ && (f & AMAP_R)) ||
            (type == MEM_WRITE && (f & AMAP_W))) {
            d->watch_triggered = true;
            d->watch_addr_hit = address;
            d->watch_read_hit = (type == MEM_READ);
            return;
        }
    }
    for (int i = 0; i < d->num_watchpoints; i++) {
        watchpoint_t* w = &d->watchpoints[i];
        if (w->addr != address) continue;
        bool hit = false;
        switch (w->mode) {
            case WATCH_WRITE:  hit = (type == MEM_WRITE); break;
            case WATCH_READ:   hit = (type == MEM_READ);  break;
            case WATCH_ACCESS: hit = true;                break;
            case WATCH_CHANGE:
                if (type == MEM_WRITE) {
                    if (!w->has_last || w->last_value != value) hit = true;
                    w->last_value = value;
                    w->has_last = true;
                }
                break;
        }
        if (hit) {
            d->watch_triggered = true;
            d->watch_addr_hit = address;
            d->watch_read_hit = (type == MEM_READ);
            return;
        }
    }
}

void debugger_install_watchpoint_trace(debugger_t* dbg, emulator_t* emu) {
    g_trace_debugger = dbg;
    /* Install when either the fixed watchpoints or the access map need it. */
    if (dbg->num_watchpoints > 0 || g_amap_rw) {
        memory_set_trace(&emu->memory, true, watchpoint_trace_callback);
    } else {
        memory_set_trace(&emu->memory, false, NULL);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SHOULD BREAK CHECK                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

bool debugger_should_break(debugger_t* dbg, emulator_t* emu) {
    uint16_t pc = emu->cpu.PC;
    dbg->last_break_reason[0] = '\0';

    /* Step mode: always break */
    if (dbg->step_mode) {
        strncpy(dbg->last_break_reason, "step", sizeof(dbg->last_break_reason) - 1);
        return true;
    }

    /* Temporary breakpoint (step-over / step-out) */
    if (dbg->has_temp_breakpoint && pc == dbg->temp_breakpoint) {
        dbg->has_temp_breakpoint = false;
        strncpy(dbg->last_break_reason, "temp", sizeof(dbg->last_break_reason) - 1);
        return true;
    }

    /* PC breakpoint hit (evaluates condition if present) */
    (void)pc;
    if (debugger_check_pc(dbg, emu)) {
        strncpy(dbg->last_break_reason, "break", sizeof(dbg->last_break_reason) - 1);
        return true;
    }

    /* Access-map execute breakpoint (US 3): PC marked with AMAP_X. */
    if ((g_amap[pc] & AMAP_X)) {
        if (!emu->control_mode)
            printf("\n*** ACCESS-MAP exec break at $%04X ***\n", pc);
        strncpy(dbg->last_break_reason, "break", sizeof(dbg->last_break_reason) - 1);
        return true;
    }

    /* Watchpoint triggered */
    if (dbg->watch_triggered) {
        if (!emu->control_mode) {
            printf("\n*** WATCHPOINT hit: %s at $%04X ***\n",
                   dbg->watch_read_hit ? "read" : "write", dbg->watch_addr_hit);
        }
        dbg->watch_triggered = false;
        strncpy(dbg->last_break_reason, "watch", sizeof(dbg->last_break_reason) - 1);
        return true;
    }

    /* Raster-line breakpoint (sprint 34d4 P2-G).
     * Fire when the current PAL line crosses a configured threshold, but
     * NOT on every single CPU step within the same line — use
     * last_raster_line to detect transitions and frame wraps. */
    if (dbg->num_raster_bps > 0) {
        int cur = emu->frame_cycles / PAL_CYCLES_PER_LINE;
        int prev = dbg->last_raster_line;
        if (cur != prev) {
            for (int i = 0; i < 8; i++) {
                int bp = dbg->raster_bps[i];
                if (bp < 0) continue;
                /* Wrap (new frame) : prev was at end, cur at start.
                 * Fire if bp lies in (prev..maxline] OR [0..cur]. */
                bool fire;
                if (prev < 0) {
                    fire = (cur == bp);
                } else if (cur < prev) {
                    fire = (bp > prev) || (bp <= cur);
                } else {
                    fire = (bp > prev && bp <= cur);
                }
                if (fire) {
                    if (!emu->control_mode) {
                        printf("\n*** RASTER BREAK at line %d (frame_cyc=%d) ***\n",
                               bp, emu->frame_cycles);
                    }
                    dbg->last_raster_line = (int16_t)cur;
                    strncpy(dbg->last_break_reason, "raster",
                            sizeof(dbg->last_break_reason) - 1);
                    return true;
                }
            }
            dbg->last_raster_line = (int16_t)cur;
        }
    }

    return false;
}

