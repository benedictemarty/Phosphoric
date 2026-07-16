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

/* Save-state is an optional link dependency: the `ss`/`sl` REPL commands use it,
 * but unit-test binaries that link debugger.c standalone must not drag in the
 * whole emulator serializer. Declared weak so those binaries still link; the
 * commands NULL-guard the calls and report gracefully when absent. */
extern bool savestate_save(const emulator_t* emu, const char* filename) __attribute__((weak));
extern bool savestate_load(emulator_t* emu, const char* filename) __attribute__((weak));

/* Parse an address argument: tries the symbol table first (case-insensitive),
 * then falls back to numeric parsing. Hex is the default base; `$`/`0x` force
 * hex and `%` forces binary. Returns true if recognised. */
static bool parse_addr(const emulator_t* emu, const char* s, uint16_t* out) {
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

static void undo_push(debugger_t* dbg, emulator_t* emu) {
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

static bool undo_pop(debugger_t* dbg, emulator_t* emu) {
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
static const char* skip_ws(const char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Advance past the current whitespace-delimited token. */
static const char* skip_token(const char* s) {
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
    const char* p = skip_ws(text);

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
        if (!parse_addr(emu, addr_buf, &addr)) return false;
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

    p = skip_ws(p);

    /* Operator */
    if (p[0] == '=' && p[1] == '=') { out->op = BP_OP_EQ; p += 2; }
    else if (p[0] == '!' && p[1] == '=') { out->op = BP_OP_NE; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { out->op = BP_OP_LE; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { out->op = BP_OP_GE; p += 2; }
    else if (p[0] == '<') { out->op = BP_OP_LT; p++; }
    else if (p[0] == '>') { out->op = BP_OP_GT; p++; }
    else return false;

    p = skip_ws(p);

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
static bool parse_condexpr(const emulator_t* emu, const char* text,
                           bp_condexpr_t* out) {
    memset(out, 0, sizeof(*out));
    const char* p = text;
    for (;;) {
        if (out->num_terms >= BP_MAX_TERMS) return false;
        const char* end = NULL;
        if (!parse_one_cond(emu, p, &out->terms[out->num_terms], &end))
            return false;
        out->num_terms++;
        p = skip_ws(end);
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
        p = skip_ws(p);
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
    if (!parse_condexpr(emu, expr, &cond)) return -2;   /* unparseable */
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
            lhs = memory_read((memory_t*)&emu->memory, c->mem_addr);
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

static const char* watch_mode_name(watch_mode_t m) {
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

/* ═══════════════════════════════════════════════════════════════════ */
/*  DISPLAY HELPERS                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

static void show_registers(emulator_t* emu) {
    char state[128];
    cpu_get_state_string(&emu->cpu, state, sizeof(state));
    const char* sym = symbol_lookup(&emu->symbols, emu->cpu.PC);
    if (sym) printf("%s  <%s>\n", state, sym);
    else     printf("%s\n", state);
}

/* Disassemble `count` instructions starting at `addr`. Returns the
 * address of the byte just past the final instruction (next page start). */
/* Sprint 34d1 P0-A — scan a disasm string for `$XXXX` operands and
 * append `; $XXXX=<name>` comments resolved through the loaded symbol
 * table. The buf itself is left untouched (width-sensitive %-18s
 * alignment) — we only print a suffix when at least one symbol matched. */
static void append_operand_symbols(const symbol_table_t* tbl, const char* buf) {
    if (!tbl || tbl->count == 0) return;
    bool printed = false;
    const char* p = buf;
    while (*p) {
        if (*p == '$') {
            const char* q = p + 1;
            int n = 0;
            unsigned int v = 0;
            while (n < 4 && isxdigit((unsigned char)*q)) {
                int d = *q;
                d = (d <= '9') ? (d - '0')
                  : ((d & 0xDF) - 'A' + 10);
                v = (v << 4) | (unsigned)d;
                q++; n++;
            }
            if (n == 4) {
                const char* s = symbol_lookup(tbl, (uint16_t)v);
                if (s) {
                    printf("%s $%04X=%s", printed ? "," : "  ;",
                           (uint16_t)v, s);
                    printed = true;
                }
                p = q;
                continue;
            }
        }
        p++;
    }
}

static uint16_t show_disassembly(emulator_t* emu, uint16_t addr, int count) {
    for (int i = 0; i < count; i++) {
        char buf[64];
        int bytes = cpu_disassemble(&emu->cpu, addr, buf, sizeof(buf));
        const char* sym = symbol_lookup(&emu->symbols, addr);
        if (sym) printf("  %s:\n", sym);
        printf("  $%04X: ", addr);
        for (int b = 0; b < 3; b++) {
            if (b < bytes)
                printf("%02X ", memory_read(&emu->memory, (uint16_t)(addr + b)));
            else
                printf("   ");
        }
        uint8_t opc = memory_read(&emu->memory, addr);
        printf(" %-18s ; %u cyc", buf, cpu_opcode_cycles(opc));
        append_operand_symbols(&emu->symbols, buf);   /* sprint 34d1 P0-A */
        if (addr == emu->cpu.PC)
            printf("  <---");
        printf("\n");
        addr = (uint16_t)(addr + bytes);
    }
    return addr;
}

static void show_memory_dump(emulator_t* emu, uint16_t addr, int len, peek_bank_t bank) {
    if (bank != PEEK_CPU)
        printf("  [bank: %s]\n", debugger_bank_name(bank));
    for (int offset = 0; offset < len; offset += 16) {
        printf("  $%04X: ", (uint16_t)(addr + offset));
        /* Hex */
        for (int i = 0; i < 16 && (offset + i) < len; i++) {
            printf("%02X ", debugger_peek_bank(emu, (uint16_t)(addr + offset + i), bank));
        }
        /* Pad if last line is short */
        int remaining = len - offset;
        if (remaining < 16) {
            for (int i = remaining; i < 16; i++)
                printf("   ");
        }
        /* ASCII */
        printf(" |");
        for (int i = 0; i < 16 && (offset + i) < len; i++) {
            uint8_t c = debugger_peek_bank(emu, (uint16_t)(addr + offset + i), bank);
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
}

static void show_stack(emulator_t* emu) {
    uint8_t sp = emu->cpu.SP;
    int depth = 0xFF - sp;
    if (depth <= 0) {
        printf("  Stack is empty (SP=$%02X)\n", sp);
        return;
    }
    if (depth > 32) depth = 32; /* Limit display */
    printf("  SP=$%02X, depth=%d bytes\n", sp, 0xFF - sp);
    printf("  $01%02X: ", (uint8_t)(sp + 1));
    for (int i = 1; i <= depth; i++) {
        printf("%02X ", memory_read(&emu->memory, (uint16_t)(0x0100 + sp + i)));
        if (i % 16 == 0 && i < depth)
            printf("\n         ");
    }
    printf("\n");
}

static void show_via_state(emulator_t* emu) {
    via6522_t* via = &emu->via;
    printf("  VIA 6522 State:\n");
    printf("    ORA=$%02X ORB=$%02X  IRA=$%02X IRB=$%02X\n",
           via->ora, via->orb, via->ira, via->irb);
    printf("    DDRA=$%02X DDRB=$%02X\n", via->ddra, via->ddrb);
    printf("    T1: counter=$%04X latch=$%04X running=%s\n",
           via->t1_counter, via->t1_latch, via->t1_running ? "yes" : "no");
    printf("    T2: counter=$%04X latch=$%02X running=%s\n",
           via->t2_counter, via->t2_latch, via->t2_running ? "yes" : "no");
    printf("    ACR=$%02X PCR=$%02X\n", via->acr, via->pcr);
    printf("    IFR=$%02X IER=$%02X  SR=$%02X\n", via->ifr, via->ier, via->sr);
    /* Decode ACR */
    printf("    ACR decode: T1=%s T2=%s SR=%d\n",
           (via->acr & 0x40) ? "free-run" : "one-shot",
           (via->acr & 0x20) ? "count-PB6" : "one-shot",
           (via->acr >> 2) & 0x07);
    /* IRQ status */
    printf("    IRQ: %s (IFR & IER = $%02X)\n",
           (via->ifr & 0x80) ? "ASSERTED" : "inactive",
           via->ifr & via->ier & 0x7F);
}

static void show_psg_state(emulator_t* emu) {
    ay3891x_t* psg = &emu->psg;
    printf("  AY-3-8910 PSG State:\n");
    printf("    Registers: ");
    for (int i = 0; i < 14; i++)
        printf("%02X ", psg->registers[i]);
    printf("\n");
    /* Decode tone periods */
    for (int ch = 0; ch < 3; ch++) {
        uint16_t period = psg->registers[ch * 2] | ((psg->registers[ch * 2 + 1] & 0x0F) << 8);
        uint8_t vol = psg->registers[8 + ch];
        bool env = (vol & 0x10) != 0;
        printf("    Chan %c: period=%4d vol=%s%d\n",
               'A' + ch, period, env ? "E" : "", vol & 0x0F);
    }
    /* Noise */
    printf("    Noise: period=%d\n", psg->registers[6] & 0x1F);
    /* Mixer */
    uint8_t mix = psg->registers[7];
    printf("    Mixer ($%02X): Tone=%c%c%c Noise=%c%c%c\n", mix,
           (mix & 0x01) ? '-' : 'A',
           (mix & 0x02) ? '-' : 'B',
           (mix & 0x04) ? '-' : 'C',
           (mix & 0x08) ? '-' : 'A',
           (mix & 0x10) ? '-' : 'B',
           (mix & 0x20) ? '-' : 'C');
    /* Envelope */
    printf("    Envelope: period=%d shape=%d step=%d vol=%d %s\n",
           psg->env_period, psg->env_shape, psg->env_step,
           psg->env_volume, psg->env_holding ? "(holding)" : "");
}

/* Sprint 34d2 P1-C — WD1793 Microdisc FDC + 4 drives mount info. */
static void show_disk_state(emulator_t* emu) {
    if (!emu->has_microdisc) {
        printf("  Microdisc: not active (use --disk-rom roms/microdis.rom)\n");
        return;
    }
    microdisc_t* md = &emu->microdisc;
    fdc_t* fdc = &md->fdc;
    printf("  Microdisc WD1793 State:\n");
    printf("    CTRL=$%02X  INTRQ=%s  DRQ=%s\n",
           md->status,
           md->intrq == 0x00 ? "asserted" : "clear",
           md->drq   == 0x00 ? "asserted" : "clear");
    printf("    Decoded: diskrom=%d romdis=%d intena=%d drive=%c side=%d\n",
           md->diskrom, md->romdis, md->intena, 'A' + md->drive, md->side);
    printf("  FDC registers:\n");
    printf("    CMD=$%02X  STATUS=$%02X  TRK=$%02X SEC=$%02X DATA=$%02X DIR=%d\n",
           fdc->command, fdc->status, fdc->track, fdc->sector,
           fdc->data, fdc->direction);
    printf("    Physical: c_track=$%02X c_sector=$%02X side=%d  cur_offset=$%04X/$%04X\n",
           fdc->c_track, fdc->c_sector, fdc->side,
           fdc->cur_offset, fdc->cur_sector_len);
    printf("    Delays:   drq=%d intrq=%d\n",
           fdc->delayed_drq, fdc->delayed_int);
    printf("  Drives:\n");
    for (int i = 0; i < 4; i++) {
        if (md->disk_data[i]) {
            printf("    %c: %u bytes, %d tracks, %d sectors\n",
                   'A' + i, md->disk_size[i],
                   md->disk_tracks[i], md->disk_sectors[i]);
        } else {
            printf("    %c: (unmounted)\n", 'A' + i);
        }
    }
}

/* Sprint 34d2 P1-D — ACIA 6551 serial registers + signals + FIFO. */
static void show_acia_state(emulator_t* emu) {
    acia6551_t* a = &emu->acia;
    printf("  ACIA 6551 State:\n");
    printf("    TDR=$%02X RDR=$%02X  STATUS=$%02X  CMD=$%02X  CTRL=$%02X\n",
           a->tdr, a->rdr, a->status, a->command, a->control);
    printf("    Frame: %u bits  baud=%u Hz  data_mask=$%02X%s\n",
           a->framebits, a->baud_rate, a->bitmask,
           a->v23_mode ? "  V23(asym 1200/75)" : "");
    printf("    Flags: tx_pending=%d rx_full=%d irq_line=%d%s\n",
           a->tx_pending, a->rx_full, a->irq_line,
           a->irq_on_rdrf ? "  (irq-on-rdrf)" : "");
    printf("    Signals: DCD=%d DSR=%d CTS=%d\n", a->dcd, a->dsr, a->cts);
    printf("    Timing: tx=%d/%d cyc  rx=%d/%d cyc\n",
           a->tx_cycles, a->tx_reload, a->rx_cycles, a->rx_reload);
    if (a->rx_fifo_size > 0) {
        printf("    RX FIFO: %d/%d bytes (head=%d tail=%d)\n",
               a->rx_fifo_count, a->rx_fifo_size,
               a->rx_fifo_head, a->rx_fifo_tail);
    } else {
        printf("    RX FIFO: disabled (1-byte mode)\n");
    }
    printf("    Backend: %s  Trace: %s\n",
           a->backend ? "attached" : "none",
           a->trace_file ? "active" : "off");
}

/* Sprint 34d2 P1-E — Cassette tape state (position, length, status). */
static void show_tape_state(emulator_t* emu) {
    printf("  Tape State:\n");
    if (!emu->tape_loaded) {
        printf("    No tape loaded (use -t FILE)\n");
        return;
    }
    int pos = emu->tapeoffs;
    int len = emu->tapelen;
    double pct = len > 0 ? 100.0 * pos / len : 0.0;
    printf("    Position: %d / %d bytes (%.1f%%)\n", pos, len, pct);
    printf("    Tape path: %s\n", emu->tape_path ? emu->tape_path : "(memory only)");
    printf("    Status: %s%s%s\n",
           emu->tape_syncstack >= 0 ? "[sync loop active] " : "",
           emu->tape_readbyte_active ? "[CLOAD reading] " : "",
           emu->fastload_pending ? "[fastload pending] " : "");
    /* Preview the next 16 bytes if available, useful to see the header type. */
    if (pos < len && emu->tapebuf) {
        int show = (len - pos) < 16 ? (len - pos) : 16;
        printf("    Next %d bytes:", show);
        for (int i = 0; i < show; i++) printf(" %02X", emu->tapebuf[pos + i]);
        printf("\n");
    }
    if (emu->csave_file) {
        printf("    CSAVE: active (capturing to .TAP file)\n");
    }
}

/* Sprint 34d3 P0-B — Full LOCI introspection (MIA register file, xstack,
 * xram windows, fd/dir tables, mount table, TAP/DSK backend, errno, top
 * ops by count). 2780 LOC of LOCI had zero debugger surface before. */
static void show_loci_state(emulator_t* emu) {
    if (!emu->has_loci) {
        printf("  LOCI: not active (use --loci or --loci-sdimg PATH)\n");
        return;
    }
    loci_t* l = &emu->loci;

    printf("  LOCI MIA State (enabled=%d):\n", l->enabled);

    /* Activity: current op + errno + return regs. */
    uint16_t err = (uint16_t)l->regs[LOCI_REG_API_ERRNO_LO]
                 | ((uint16_t)l->regs[LOCI_REG_API_ERRNO_HI] << 8);
    printf("    active_op=$%02X  errno=%u  A=$%02X X=$%02X "
           "SREG=$%02X%02X  BUSY=$%02X\n",
           l->active_op, err,
           l->regs[LOCI_REG_API_A], l->regs[LOCI_REG_API_X],
           l->regs[LOCI_REG_API_SREG_HI], l->regs[LOCI_REG_API_SREG],
           l->regs[LOCI_REG_BUSY]);

    /* Top 5 ops by count. */
    uint64_t total = 0;
    int top[5] = {-1, -1, -1, -1, -1};
    for (int op = 1; op < 256; op++) {
        total += l->op_count[op];
        for (int s = 0; s < 5; s++) {
            if (top[s] < 0 ||
                l->op_count[op] > l->op_count[top[s]]) {
                for (int m = 4; m > s; m--) top[m] = top[m - 1];
                top[s] = op;
                break;
            }
        }
    }
    printf("    Ops dispatched: %llu total\n", (unsigned long long)total);
    if (total > 0) {
        printf("    Top:");
        for (int s = 0; s < 5 && top[s] >= 0 &&
                       l->op_count[top[s]] > 0; s++) {
            printf("  $%02X×%llu", (uint8_t)top[s],
                   (unsigned long long)l->op_count[top[s]]);
        }
        printf("\n");
    }

    /* xstack — print pointer + a sample of the top bytes. */
    printf("    xstack_ptr=$%04X (used %u/%u):", l->xstack_ptr,
           LOCI_XSTACK_SIZE - l->xstack_ptr, LOCI_XSTACK_SIZE);
    int used = LOCI_XSTACK_SIZE - l->xstack_ptr;
    int show = used < 16 ? used : 16;
    for (int i = 0; i < show; i++)
        printf(" %02X", l->xstack[l->xstack_ptr + i]);
    if (used > 16) printf(" …");
    printf("\n");

    /* xram windows. */
    uint16_t a0 = (uint16_t)l->regs[LOCI_REG_ADDR0_LO]
                | ((uint16_t)l->regs[LOCI_REG_ADDR0_HI] << 8);
    uint16_t a1 = (uint16_t)l->regs[LOCI_REG_ADDR1_LO]
                | ((uint16_t)l->regs[LOCI_REG_ADDR1_HI] << 8);
    int8_t s0 = (int8_t)l->regs[LOCI_REG_STEP0];
    int8_t s1 = (int8_t)l->regs[LOCI_REG_STEP1];
    printf("    xram window0 addr=$%04X step=%+d   window1 addr=$%04X step=%+d\n",
           a0, s0, a1, s1);
    printf("    HID xram: kbd=$%04X mou=$%04X pad=$%04X\n",
           l->kbd_xram, l->mou_xram, l->pad_xram);

    /* Sandbox + SDIMG backend. */
    printf("    flash_root=\"%s\"  sdimg=%s\n",
           l->flash_root[0] ? l->flash_root : "(cwd)",
           l->sdimg ? "attached" : "none");

    /* File handles. */
    int fd_active = 0;
    for (int i = 0; i < LOCI_FD_MAX; i++)
        if (l->fd_kind[i] != 0) fd_active++;
    if (fd_active > 0) {
        printf("    File handles: %d/%d open\n", fd_active, LOCI_FD_MAX);
        for (int i = 0; i < LOCI_FD_MAX; i++) {
            if (l->fd_kind[i] != 0) {
                printf("      fd=%d  %s\n", i + LOCI_FD_OFFSET,
                       l->fd_kind[i] == 1 ? "POSIX" :
                       l->fd_kind[i] == 2 ? "SDIMG" : "?");
            }
        }
    } else {
        printf("    File handles: 0 open\n");
    }

    /* Dir handles. */
    int dir_active = 0;
    for (int i = 0; i < LOCI_DIR_MAX; i++)
        if (l->dir_kind[i] != 0) dir_active++;
    if (dir_active > 0) {
        printf("    Dir handles: %d/%d open\n", dir_active, LOCI_DIR_MAX);
        for (int i = 0; i < LOCI_DIR_MAX; i++) {
            if (l->dir_kind[i] != 0) {
                printf("      dir_fd=%d  path=\"%s\"\n",
                       i + LOCI_DIR_OFFSET,
                       l->dirs_path[i][0] ? l->dirs_path[i] : "(none)");
            }
        }
    } else {
        printf("    Dir handles: 0 open\n");
    }

    /* Mount table. */
    static const char* mnt_label[LOCI_MNT_MAX] = {
        "drive A", "drive B", "drive C", "drive D", "TAP", "ROM" };
    int mnt_active = 0;
    for (int i = 0; i < LOCI_MNT_MAX; i++)
        if (l->mnt_mounted[i]) mnt_active++;
    if (mnt_active > 0) {
        printf("    Mounts: %d/%d active\n", mnt_active, LOCI_MNT_MAX);
        for (int i = 0; i < LOCI_MNT_MAX; i++) {
            if (l->mnt_mounted[i]) {
                printf("      [%d %-8s] %s\n", i, mnt_label[i],
                       l->mnt_paths[i][0] ? l->mnt_paths[i] : "(no path)");
            }
        }
    } else {
        printf("    Mounts: 0 active\n");
    }

    /* TAP backend. */
    if (l->tap_fp) {
        double pct = l->tap_size > 0 ? 100.0 * l->tap_counter / l->tap_size : 0.0;
        printf("    TAP: %u/%u bytes (%.1f%%) cmd=$%02X stat=$%02X\n",
               l->tap_counter, l->tap_size, pct, l->tap_cmd, l->tap_stat);
    } else {
        printf("    TAP: idle\n");
    }

    /* DSK bus selection + per-drive status. */
    printf("    DSK selected=%c  CTRL=$%02X  INTRQ=%s INTENA=%d\n",
           'A' + l->dsk_selected, l->dsk_ctrl,
           l->dsk_intrq == 0x00 ? "asserted" : "clear",
           l->dsk_intena);
    for (int i = 0; i < 4; i++) {
        if (l->dsk_image[i]) {
            printf("      %c: %u bytes, %d tracks, %d sec/track  %s%s\n",
                   'A' + i, l->dsk_image_size[i],
                   l->dsk_tracks[i], l->dsk_sectors[i],
                   l->dsk_is_mfm[i] ? "MFM" : "raw",
                   l->dsk_host_path[i][0] ? "" : " (no path)");
        }
    }

    /* Boot settings (last MIA_BOOT). */
    if (l->boot_settings) {
        printf("    boot_settings=$%02X:%s%s%s%s%s%s%s\n", l->boot_settings,
               (l->boot_settings & LOCI_BOOT_FDC)     ? " FDC"     : "",
               (l->boot_settings & LOCI_BOOT_TAP)     ? " TAP"     : "",
               (l->boot_settings & LOCI_BOOT_B11)     ? " B11"     : "",
               (l->boot_settings & LOCI_BOOT_TAP_BIT) ? " TAP_BIT" : "",
               (l->boot_settings & LOCI_BOOT_TAP_ALD) ? " TAP_ALD" : "",
               (l->boot_settings & LOCI_BOOT_RESUME)  ? " RESUME"  : "",
               (l->boot_settings & LOCI_BOOT_FAST)    ? " FAST"    : "");
    }
}

/* US 5 — élargissement de la couverture d'inspection (parité fenêtres b2). */
static void show_video_state(emulator_t* emu) {
    video_t* v = &emu->video;
    printf("  ULA / Video State:\n");
    printf("    mode=%s  vid_mode=$%02X  text_attr=$%02X  need_refresh=%s\n",
           v->hires_mode ? "HIRES" : "TEXT", v->vid_mode, v->text_attr,
           v->need_refresh ? "yes" : "no");
    printf("    framebuffer=%dx%d  frame_counter=%u\n",
           v->native_w, v->native_h, v->frame_counter);
}

static void show_keyboard_state(emulator_t* emu) {
    oric_keyboard_t* k = &emu->keyboard;
    printf("  Keyboard (8 columns, ORB[0:2] selects col; rows active-low):\n");
    printf("    col:     0    1    2    3    4    5    6    7\n");
    printf("    matrix: ");
    for (int c = 0; c < 8; c++) printf("$%02X  ", k->matrix[c]);
    printf("\n");
    printf("    layout=%d\n", (int)k->layout);
#ifdef HAS_SDL2
    printf("    pressed_count=%d  pending=%s (scancode=%u)\n",
           k->pressed_count, k->has_pending ? "yes" : "no",
           (unsigned)k->pending_scancode);
#endif
}

static void show_joystick_state(emulator_t* emu) {
    oric_joystick_t* j = &emu->joystick;
    const char* mode = j->mode == ORIC_JOY_DISABLED    ? "disabled"    :
                       j->mode == ORIC_JOY_SDL_GAMEPAD ? "sdl-gamepad" :
                       j->mode == ORIC_JOY_KEYBOARD    ? "keyboard"    : "?";
    printf("  IJK Joystick State:\n");
    printf("    mode=%s  port_a_mask=$%02X (active-low)  interface present=%s\n",
           mode, j->port_a_mask, (j->port_a_mask & IJK_PRESENCE) ? "no" : "yes");
#ifdef HAS_SDL2
    printf("    device_index=%d\n", j->device_index);
#endif
}

static void show_printer_state(emulator_t* emu) {
    oric_printer_t* p = &emu->printer;
    const char* type = p->type == PRINTER_NONE  ? "none"  :
                       p->type == PRINTER_TEXT  ? "text"  :
                       p->type == PRINTER_MCP40 ? "mcp40" : "?";
    printf("  Printer State:\n");
    printf("    type=%s  output=%s  strobe_low=%s  byte_count=%u\n",
           type, p->filename ? p->filename : "(none)",
           p->strobe_low ? "yes" : "no", (unsigned)p->byte_count);
    if (p->type == PRINTER_MCP40) {
        mcp40_t* m = &p->mcp40;
        printf("    MCP-40: pen=(%d,%d) color=%d char_size=%d lines=%u chars=%u dirty=%s\n",
               m->pen_x, m->pen_y, (int)m->color, m->char_size,
               (unsigned)m->line_count, (unsigned)m->char_count,
               m->dirty ? "yes" : "no");
    }
}

static void show_help(void) {
    printf("\n  Debugger Commands:\n");
    printf("  ─────────────────────────────────────────────────\n");
    printf("  s / step          Step 1 instruction\n");
    printf("  n / next          Step over (JSR → break at return)\n");
    printf("  c / continue      Continue execution\n");
    printf("  u / undo          Rewind last step (CPU+RAM, ring of 16)\n");
    printf("  r / regs          Show CPU registers\n");
    printf("  d                 Disassemble next page (page size persists)\n");
    printf("  d addr [n]        Jump-disasm; push current page to history\n");
    printf("  d +               Same as `d` (next page)\n");
    printf("  d -               Pop history (previous page)\n");
    printf("  m addr [len] [bank]  Memory dump hex+ASCII (bank: cpu|ram|rom|overlay)\n");
    printf("  m addr = V1 [V2...]  Write byte(s) to memory\n");
    printf("  a addr MNE [op]   Assemble one instruction in place (e.g. a 0400 LDA #$41)\n");
    printf("  find B1 [B2...]   Search memory for a hex byte pattern\n");
    printf("  find \"text\"       Search memory for an ASCII string\n");
    printf("  b addr            Add PC breakpoint\n");
    printf("  b addr if EXPR    Conditional breakpoint\n");
    printf("                    EXPR: TERM [&&|| TERM]... (up to 4 terms)\n");
    printf("                    TERM: REG op VAL | M[ADDR] op VAL\n");
    printf("                    REG: A X Y SP P PC   op: == != < <= > >=\n");
    printf("  b                 List all breakpoints\n");
    printf("  bd n              Delete breakpoint #n\n");
    printf("  br line           Raster bp at PAL line (0..311)\n");
    printf("  br                List raster breakpoints\n");
    printf("  brd n             Delete raster bp #n (or `brd *` to clear all)\n");
    printf("  w addr [w|r|a|c]  Add watchpoint: write/read/access/change (default write)\n");
    printf("  w                 List all watchpoints\n");
    printf("  wd n              Delete watchpoint #n\n");
    printf("  wr START END [rwx]  Access-map region breakpoint (default rw) | wr | wr clear\n");
    printf("  hunt              Start cheat-finder (all cells candidate)\n");
    printf("  hunt V | = | + | - | !   Narrow: ==V / unchanged / up / down / changed\n");
    printf("  hunt list | clear Show candidates / reset the hunt\n");
    printf("  via               Show VIA 6522 state\n");
    printf("  psg               Show PSG AY-3-8910 state\n");
    printf("  disk / fdc        Show Microdisc WD1793 + 4 drives\n");
    printf("  acia / serial     Show ACIA 6551 registers + signals + FIFO\n");
    printf("  tape / cassette   Show tape position, status, next bytes\n");
    printf("  loci              Show LOCI MIA full state (regs, fds, mounts, DSK/TAP)\n");
    printf("  video / ula       Show ULA/video state (mode, framebuffer)\n");
    printf("  kbd               Show keyboard matrix (8 columns)\n");
    printf("  joy               Show IJK joystick state\n");
    printf("  printer / mcp40   Show printer / MCP-40 plotter state\n");
    printf("  stack             Show stack contents\n");
    printf("  set reg val       Set register (A,X,Y,SP,PC,P)\n");
    printf("  set via reg val   Set VIA 6522 register (0..15)\n");
    printf("  stuck [S0 [S1]]   RAM stuck-bit fault injection (S0→0, S1→1; 0 0 = off)\n");
    printf("  save FILE a len   Write memory region to a binary file\n");
    printf("  load FILE addr    Read a binary file into memory\n");
    printf("  disf FILE a n     Disassemble n instructions to a file\n");
    printf("  ss FILE / sl FILE Save / load full machine state (.ost)\n");
    printf("  trace start ...   Conditional trace: [now|pc:HEX] [stop:cycle:N|brk|write:HEX|read:HEX] [ring:N] [sym]\n");
    printf("  trace stop|save FILE|status|off\n");
    printf("  sym [name|addr]   List symbols / resolve name or address\n");
    printf("  sym load FILE [g] | sym group N on|off | sym groups   Symbol groups (US 4)\n");
    printf("  (numbers: hex default, $ hex, %% binary; symbols if --symbols loaded)\n");
    printf("  q / quit          Quit emulator\n");
    printf("  h / help          Show this help\n");
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MEMORY SEARCH (find)                                               */
/* ═══════════════════════════════════════════════════════════════════ */

/* Side-effect-free memory peek: read the backing RAM array for $0000-$BFFF
 * (so searching never touches VIA/ACIA I/O registers and clears flags), and
 * the side-effect-free CPU view (ROM/overlay) for $C000-$FFFF. */
static uint8_t dbg_peek(emulator_t* emu, uint16_t a) {
    if (a < RAM_SIZE) return emu->memory.ram[a];
    return memory_read(&emu->memory, a);
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
static bool     hunt_cand[0x10000];
static uint8_t  hunt_prev[0x10000];
static bool     hunt_active = false;
static uint32_t hunt_count  = 0;

static void hunt_snapshot(emulator_t* emu) {
    for (uint32_t a = 0; a < 0x10000; a++)
        hunt_prev[a] = dbg_peek(emu, (uint16_t)a);
}

/* Begin a hunt: every address becomes a candidate. */
void debugger_hunt_start(emulator_t* emu) {
    for (uint32_t a = 0; a < 0x10000; a++) hunt_cand[a] = true;
    hunt_count = 0x10000;
    hunt_snapshot(emu);
    hunt_active = true;
}

uint32_t debugger_hunt_count(void) { return hunt_count; }
bool     debugger_hunt_active(void) { return hunt_active; }
void     debugger_hunt_clear(void) { hunt_active = false; hunt_count = 0; }

uint32_t debugger_hunt_list(emulator_t* emu, uint16_t* out, uint32_t max, uint8_t* out_vals) {
    uint32_t n = 0;
    for (uint32_t a = 0; a < 0x10000 && n < max; a++) {
        if (!hunt_cand[a]) continue;
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
        if (!hunt_cand[a]) continue;
        uint8_t cur = dbg_peek(emu, (uint16_t)a);
        bool keep = false;
        switch (pred) {
            case HUNT_EQ:        keep = (cur == val);          break;
            case HUNT_UNCHANGED: keep = (cur == hunt_prev[a]); break;
            case HUNT_CHANGED:   keep = (cur != hunt_prev[a]); break;
            case HUNT_GT:        keep = (cur >  hunt_prev[a]); break;
            case HUNT_LT:        keep = (cur <  hunt_prev[a]); break;
        }
        if (keep) kept++; else hunt_cand[a] = false;
    }
    hunt_count = kept;
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
static int assemble_one(emulator_t* emu, uint16_t addr,
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

/* ═══════════════════════════════════════════════════════════════════ */
/*  REPL COMMAND LOOP                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

/* Single-line REPL command dispatch. Used by the interactive REPL
 * loop and by the TUI's ':' command-line mode. */
static void process_repl_line(debugger_t* dbg, emulator_t* emu, const char* line) {
        /* Parse command */
        char cmd[32] = {0};
        char arg1[32] = {0};
        char arg2[32] = {0};
        sscanf(line, "%31s %31s %31s", cmd, arg1, arg2);

        /* ── STEP ───────────────────────────────────────── */
        if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0) {
            undo_push(dbg, emu);   /* sprint 34d5 P2-F */
            dbg->step_mode = true;
            dbg->active = false;
            /* Execute one instruction and come back */
        }
        /* ── NEXT (step-over) ───────────────────────────── */
        else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
            undo_push(dbg, emu);   /* sprint 34d5 P2-F */
            /* Check if current instruction is JSR ($20) */
            uint8_t opcode = memory_read(&emu->memory, emu->cpu.PC);
            if (opcode == 0x20) {
                /* JSR abs: set temp breakpoint at PC+3 */
                dbg->temp_breakpoint = (uint16_t)(emu->cpu.PC + 3);
                dbg->has_temp_breakpoint = true;
                dbg->step_mode = false;
            } else {
                /* Not JSR: just step */
                dbg->step_mode = true;
            }
            dbg->active = false;
        }
        /* ── UNDO (sprint 34d5 P2-F) ───────────────────── */
        else if (strcmp(cmd, "u") == 0 || strcmp(cmd, "undo") == 0) {
            if (undo_pop(dbg, emu)) {
                printf("  Rewound 1 step. PC=$%04X cycles=%llu "
                       "(%u snapshots left)\n",
                       emu->cpu.PC, (unsigned long long)emu->cpu.cycles,
                       dbg->undo_count);
                show_disassembly(emu, emu->cpu.PC, 1);
            } else {
                printf("  Nothing to undo (ring empty)\n");
            }
        }
        /* ── CONTINUE ───────────────────────────────────── */
        else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) {
            dbg->step_mode = false;
            dbg->active = false;
        }
        /* ── REGISTERS ──────────────────────────────────── */
        else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "regs") == 0) {
            show_registers(emu);
        }
        /* ── SYMBOLS ────────────────────────────────────── */
        else if (strcmp(cmd, "sym") == 0) {
            char a3[32] = {0};
            sscanf(line, "%*s %*s %*s %31s", a3);
            if (strcasecmp(arg1, "load") == 0) {
                /* sym load FILE [group] */
                if (!arg2[0]) { printf("  Usage: sym load FILE [group]\n"); return; }
                uint8_t grp = (uint8_t)strtoul(a3[0] ? a3 : "0", NULL, 0);
                int n = symbol_table_load_group(&emu->symbols, arg2, grp);
                if (n < 0) printf("  Failed to load %s\n", arg2);
                else printf("  Loaded %d symbols from %s (group %u)\n", n, arg2, grp);
            } else if (strcasecmp(arg1, "group") == 0) {
                /* sym group N on|off */
                if (!arg2[0] || !a3[0]) { printf("  Usage: sym group N on|off\n"); return; }
                uint8_t grp = (uint8_t)strtoul(arg2, NULL, 0);
                bool en = (strcasecmp(a3, "on") == 0 || strcmp(a3, "1") == 0);
                symbol_set_group_enabled(&emu->symbols, grp, en);
                printf("  Group %u %s (%d symbols)\n", grp, en ? "enabled" : "disabled",
                       symbol_group_count(&emu->symbols, grp));
            } else if (strcasecmp(arg1, "groups") == 0) {
                printf("  Symbol groups (non-empty):\n");
                int any = 0;
                for (int g = 0; g < 256; g++) {
                    int c = symbol_group_count(&emu->symbols, (uint8_t)g);
                    if (c > 0) {
                        printf("    group %d: %d symbols  [%s]\n", g, c,
                               symbol_group_enabled(&emu->symbols, (uint8_t)g) ? "on" : "off");
                        any++;
                    }
                }
                if (!any) printf("    (none)\n");
            } else if (!arg1[0]) {
                if (emu->symbols.count == 0) {
                    printf("  No symbols loaded (use --symbols FILE or `sym load FILE [g]`)\n");
                } else {
                    printf("  %d symbols loaded\n", emu->symbols.count);
                    int show = emu->symbols.count > 20 ? 20 : emu->symbols.count;
                    for (int i = 0; i < show; i++)
                        printf("    $%04X  %s\n",
                               emu->symbols.entries[i].addr,
                               emu->symbols.entries[i].name);
                    if (emu->symbols.count > show)
                        printf("    … (%d more)\n", emu->symbols.count - show);
                }
            } else {
                uint16_t addr;
                if (parse_addr(emu, arg1, &addr)) {
                    const char* s = symbol_lookup(&emu->symbols, addr);
                    if (s) printf("  $%04X = %s\n", addr, s);
                    else   printf("  $%04X = (no symbol)\n", addr);
                } else {
                    printf("  Unknown symbol: %s\n", arg1);
                }
            }
        }
        /* ── DISASSEMBLE (paginated) ────────────────────── */
        else if (strcmp(cmd, "d") == 0) {
            /* Initialise default page size on first use this session. */
            if (dbg->disasm_count == 0) dbg->disasm_count = 10;

            int count = dbg->disasm_count;
            bool go_back = false;
            uint16_t addr = dbg->disasm_cursor_valid ? dbg->disasm_cursor
                                                     : emu->cpu.PC;

            if (arg1[0] == '-' && arg1[1] == '\0') {
                go_back = true;
            } else if (arg1[0] == '+' && arg1[1] == '\0') {
                /* "next page" — same as no arg */
            } else if (arg1[0]) {
                uint16_t a;
                if (parse_addr(emu, arg1, &a)) {
                    addr = a;
                } else {
                    printf("  Unknown address/symbol: %s\n", arg1);
                    return;
                }
            }
            if (arg2[0]) {
                int c = atoi(arg2);
                if (c >= 1 && c <= 100) {
                    count = c;
                    dbg->disasm_count = (uint8_t)c;
                }
            }

            if (go_back) {
                /* Need at least 2 entries: top is current page start,
                 * the one before is the previous one. */
                if (dbg->disasm_history_top < 2) {
                    printf("  (no previous page)\n");
                    return;
                }
                dbg->disasm_history_top--;   /* discard current */
                addr = dbg->disasm_history[--dbg->disasm_history_top];
            }

            /* Push this page's start address to history (ring of 16). */
            if (dbg->disasm_history_top < 16) {
                dbg->disasm_history[dbg->disasm_history_top++] = addr;
            } else {
                for (int i = 1; i < 16; i++)
                    dbg->disasm_history[i - 1] = dbg->disasm_history[i];
                dbg->disasm_history[15] = addr;
            }

            uint16_t next = show_disassembly(emu, addr, count);
            dbg->disasm_cursor = next;
            dbg->disasm_cursor_valid = true;
        }
        /* ── MEMORY DUMP / WRITE ────────────────────────── */
        else if (strcmp(cmd, "m") == 0) {
            if (!arg1[0]) {
                printf("  Usage: m addr [len] [bank]   dump memory (bank: cpu|ram|rom|overlay)\n");
                printf("         m addr = V1 [V2 ...]  write byte(s)\n");
                return;
            }
            uint16_t addr;
            if (!parse_addr(emu, arg1, &addr)) {
                printf("  Unknown address/symbol: %s\n", arg1);
                return;
            }
            /* Detect "=" in arg2 (separator) or as part of arg2 like "=42".
             * Anything after = is a list of values; we re-scan `line` after
             * the '=' character to read more than the 3 tokens sscanf got. */
            const char* eq = strchr(line, '=');
            if (eq) {
                const char* vals = skip_ws(eq + 1);
                int written = 0;
                uint16_t write_addr = addr;
                while (*vals && written < 256) {
                    char tok[32];
                    size_t tn = 0;
                    while (*vals && !isspace((unsigned char)*vals) &&
                           *vals != ',' && tn < sizeof(tok) - 1) {
                        tok[tn++] = *vals++;
                    }
                    tok[tn] = '\0';
                    if (tn == 0) { vals = skip_ws(vals); continue; }
                    /* Each token: hex with $/0x prefix, else decimal */
                    const char* s = tok;
                    int base = 10;
                    if (*s == '$') { s++; base = 16; }
                    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
                    else if (tn >= 2) {
                        /* Heuristic: if any alpha hex digit appears, treat as hex */
                        for (size_t i = 0; i < tn; i++) {
                            char c = tok[i];
                            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                                base = 16;
                                break;
                            }
                        }
                    }
                    char* end = NULL;
                    unsigned long v = strtoul(s, &end, base);
                    if (end == s || v > 0xFF) {
                        printf("  Invalid byte value: %s\n", tok);
                        break;
                    }
                    memory_write(&emu->memory, write_addr, (uint8_t)v);
                    write_addr = (uint16_t)(write_addr + 1);
                    written++;
                    vals = skip_ws(vals);
                    while (*vals == ',') vals = skip_ws(vals + 1);
                }
                if (written > 0) {
                    printf("  Wrote %d byte%s to $%04X-$%04X\n",
                           written, written > 1 ? "s" : "",
                           addr, (uint16_t)(addr + written - 1));
                }
            } else {
                /* Optional trailing tokens: [len] [bank]. `arg2` may be either
                 * a length or a bank name; a 4th token (if present) is a bank. */
                peek_bank_t bank = PEEK_CPU;
                int len2 = 256;
                if (arg2[0]) {
                    if (debugger_parse_bank(arg2, &bank)) {
                        len2 = 256;   /* "m addr rom" — default length */
                    } else {
                        len2 = (int)strtol(arg2, NULL, 0);
                        char a3[16] = {0};
                        if (sscanf(line, "%*s %*s %*s %15s", a3) == 1 &&
                            !debugger_parse_bank(a3, &bank)) {
                            printf("  Unknown bank '%s' (cpu|ram|rom|overlay)\n", a3);
                            return;
                        }
                    }
                }
                if (len2 < 1) len2 = 1;
                if (len2 > 65536) len2 = 65536;
                show_memory_dump(emu, addr, len2, bank);
            }
        }
        /* ── MEMORY SEARCH ──────────────────────────────── */
        else if (strcmp(cmd, "find") == 0) {
            const char* p = skip_ws(skip_token(skip_ws(line)));  /* after "find" */
            if (!*p) {
                printf("  Usage: find B1 [B2 ...]   search byte pattern (hex)\n");
                printf("         find \"text\"         search ASCII string\n");
                return;
            }
            uint8_t pat[64];
            int plen = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && plen < (int)sizeof(pat))
                    pat[plen++] = (uint8_t)*p++;
            } else {
                while (*p && plen < (int)sizeof(pat)) {
                    char tok[16];
                    size_t tn = 0;
                    while (*p && !isspace((unsigned char)*p) && tn < sizeof(tok) - 1)
                        tok[tn++] = *p++;
                    tok[tn] = '\0';
                    const char* s = (tok[0] == '$') ? tok + 1 : tok;
                    char* end = NULL;
                    unsigned long v = strtoul(s, &end, 16);
                    if (end == s || *end || v > 0xFF) {
                        printf("  Invalid hex byte: %s\n", tok);
                        return;
                    }
                    pat[plen++] = (uint8_t)v;
                    p = skip_ws(p);
                }
            }
            if (plen == 0) { printf("  Empty search pattern\n"); return; }

            int found = 0;
            const int limit = 64;
            for (uint32_t a = 0; a + (uint32_t)plen <= 0x10000; a++) {
                int j = 0;
                for (; j < plen; j++)
                    if (dbg_peek(emu, (uint16_t)(a + (uint32_t)j)) != pat[j]) break;
                if (j != plen) continue;
                if (found < limit) {
                    const char* s = symbol_lookup(&emu->symbols, (uint16_t)a);
                    printf("    $%04X%s%s\n", (uint16_t)a, s ? "  " : "", s ? s : "");
                }
                found++;
            }
            if (found == 0) {
                printf("  Pattern not found (%d byte%s)\n",
                       plen, plen > 1 ? "s" : "");
            } else {
                printf("  %d match%s%s\n", found, found > 1 ? "es" : "",
                       found > limit ? " (showing first 64)" : "");
            }
        }
        /* ── INLINE ASSEMBLER ───────────────────────────── */
        else if (strcmp(cmd, "a") == 0) {
            if (!arg1[0] || !arg2[0]) {
                printf("  Usage: a addr MNEMONIC [operand]\n");
                printf("         e.g. a 0400 LDA #$41   a 0402 STA $BB80   a 0405 BNE 0400\n");
                return;
            }
            uint16_t addr;
            if (!parse_addr(emu, arg1, &addr)) {
                printf("  Unknown address/symbol: %s\n", arg1);
                return;
            }
            /* Operand = raw text following the mnemonic token (arg2 holds the
             * mnemonic). Walk past cmd, addr and mnemonic in `line`. */
            const char* p = skip_ws(skip_token(skip_ws(line)));   /* after cmd */
            p = skip_ws(skip_token(p));                            /* after addr */
            p = skip_ws(skip_token(p));                            /* after mnemonic */
            int n = assemble_one(emu, addr, arg2, p);
            if (n > 0) {
                char dis[80];
                cpu_disassemble(&emu->cpu, addr, dis, sizeof(dis));
                printf("  $%04X: %s\n", addr, dis);
                dbg->disasm_cursor = (uint16_t)(addr + n);
                dbg->disasm_cursor_valid = true;
            }
        }
        /* ── BREAKPOINT ─────────────────────────────────── */
        else if (strcmp(cmd, "b") == 0) {
            if (!arg1[0]) {
                if (dbg->num_breakpoints == 0) {
                    printf("  No breakpoints set\n");
                } else {
                    printf("  Breakpoints:\n");
                    for (int i = 0; i < dbg->num_breakpoints; i++) {
                        breakpoint_t* bp = &dbg->breakpoints[i];
                        const char* s = symbol_lookup(&emu->symbols, bp->addr);
                        printf("    #%d: $%04X", i, bp->addr);
                        if (s) printf("  %s", s);
                        if (bp->has_cond) printf("  if %s", bp->cond_text);
                        printf("\n");
                    }
                }
            } else {
                uint16_t addr;
                if (!parse_addr(emu, arg1, &addr)) {
                    printf("  Unknown address/symbol: %s\n", arg1);
                    return;
                }
                /* Detect optional "if <expression>" after the address.
                 * sscanf collected the address into arg1 and the next
                 * token into arg2; everything else lives in `line` after
                 * the prefix "<cmd> <addr> if ". Find that "if". */
                const char* if_pos = NULL;
                if (strncasecmp(arg2, "if", 2) == 0 && !arg2[2]) {
                    /* arg2 is literally "if" — find it in the raw line. */
                    const char* p = strstr(line, " if ");
                    if (p) if_pos = skip_ws(p + 4);
                }
                int idx = debugger_add_breakpoint(dbg, addr);
                if (idx < 0) {
                    printf("  Error: maximum breakpoints reached (%d)\n",
                           DEBUGGER_MAX_BREAKPOINTS);
                    return;
                }
                if (if_pos && *if_pos) {
                    bp_condexpr_t cond;
                    if (!parse_condexpr(emu, if_pos, &cond)) {
                        debugger_remove_breakpoint(dbg, idx);
                        printf("  Invalid condition: %s\n", if_pos);
                        printf("  Syntax: TERM [ && | || TERM ]...  (up to %d terms)\n",
                               BP_MAX_TERMS);
                        printf("  TERM: REG op VALUE  or  M[ADDR] op VALUE\n");
                        printf("  REG: A X Y SP P PC   op: == != < <= > >=\n");
                        return;
                    }
                    breakpoint_t* bp = &dbg->breakpoints[idx];
                    bp->has_cond = true;
                    bp->cond = cond;
                    strncpy(bp->cond_text, if_pos, sizeof(bp->cond_text) - 1);
                    bp->cond_text[sizeof(bp->cond_text) - 1] = '\0';
                    /* Strip trailing newline / whitespace */
                    size_t cl = strlen(bp->cond_text);
                    while (cl > 0 && isspace((unsigned char)bp->cond_text[cl-1]))
                        bp->cond_text[--cl] = '\0';
                    printf("  Breakpoint #%d set at $%04X if %s\n",
                           idx, addr, bp->cond_text);
                } else {
                    printf("  Breakpoint #%d set at $%04X\n", idx, addr);
                }
            }
        }
        /* ── BREAKPOINT DELETE ──────────────────────────── */
        else if (strcmp(cmd, "bd") == 0) {
            if (!arg1[0]) {
                printf("  Usage: bd <index>\n");
            } else {
                int idx = atoi(arg1);
                if (debugger_remove_breakpoint(dbg, idx))
                    printf("  Breakpoint #%d removed\n", idx);
                else
                    printf("  Invalid breakpoint index\n");
            }
        }
        /* ── RASTER-LINE BREAKPOINT (sprint 34d4 P2-G) ── */
        else if (strcmp(cmd, "br") == 0) {
            if (!arg1[0]) {
                int n = 0;
                for (int i = 0; i < 8; i++) {
                    if (dbg->raster_bps[i] >= 0) {
                        printf("  #%d: line %d\n", i, dbg->raster_bps[i]);
                        n++;
                    }
                }
                if (n == 0) printf("  No raster breakpoints (range: 0-311)\n");
            } else {
                int line = atoi(arg1);
                if (line < 0 || line >= PAL_LINES_PER_FRAME) {
                    printf("  Line must be 0..%d\n", PAL_LINES_PER_FRAME - 1);
                } else {
                    int slot = -1;
                    for (int i = 0; i < 8; i++) {
                        if (dbg->raster_bps[i] < 0) { slot = i; break; }
                    }
                    if (slot < 0) {
                        printf("  All 8 raster breakpoint slots used\n");
                    } else {
                        dbg->raster_bps[slot] = (int16_t)line;
                        dbg->num_raster_bps++;
                        printf("  Raster breakpoint #%d set at line %d\n",
                               slot, line);
                    }
                }
            }
        }
        else if (strcmp(cmd, "brd") == 0) {
            if (!arg1[0]) {
                printf("  Usage: brd <index>  (or `brd *` to clear all)\n");
            } else if (arg1[0] == '*') {
                for (int i = 0; i < 8; i++) dbg->raster_bps[i] = -1;
                dbg->num_raster_bps = 0;
                printf("  All raster breakpoints cleared\n");
            } else {
                int idx = atoi(arg1);
                if (idx < 0 || idx >= 8 || dbg->raster_bps[idx] < 0) {
                    printf("  Invalid raster breakpoint index\n");
                } else {
                    dbg->raster_bps[idx] = -1;
                    dbg->num_raster_bps--;
                    printf("  Raster breakpoint #%d removed\n", idx);
                }
            }
        }
        /* ── WATCHPOINT ─────────────────────────────────── */
        else if (strcmp(cmd, "w") == 0) {
            if (!arg1[0]) {
                /* List watchpoints */
                if (dbg->num_watchpoints == 0) {
                    printf("  No watchpoints set\n");
                } else {
                    printf("  Watchpoints:\n");
                    for (int i = 0; i < dbg->num_watchpoints; i++) {
                        const watchpoint_t* w = &dbg->watchpoints[i];
                        const char* s = symbol_lookup(&emu->symbols, w->addr);
                        printf("    #%d: $%04X (%s)%s%s\n", i, w->addr,
                               watch_mode_name(w->mode), s ? "  " : "", s ? s : "");
                    }
                }
            } else {
                uint16_t addr;
                if (!parse_addr(emu, arg1, &addr)) {
                    printf("  Unknown address/symbol: %s\n", arg1);
                    return;
                }
                /* Optional mode token: w (write, default), r (read),
                 * a (access = read|write), c (change). */
                watch_mode_t mode = WATCH_WRITE;
                if (arg2[0]) {
                    switch (tolower((unsigned char)arg2[0])) {
                        case 'w': mode = WATCH_WRITE;  break;
                        case 'r': mode = WATCH_READ;   break;
                        case 'a': mode = WATCH_ACCESS; break;
                        case 'c': mode = WATCH_CHANGE; break;
                        default:
                            printf("  Unknown mode '%s' (use w|r|a|c)\n", arg2);
                            return;
                    }
                }
                int idx = debugger_add_watchpoint_mode(dbg, addr, mode);
                if (idx >= 0) {
                    printf("  Watchpoint #%d set at $%04X (%s)\n",
                           idx, addr, watch_mode_name(mode));
                    debugger_install_watchpoint_trace(dbg, emu);
                } else {
                    printf("  Error: maximum watchpoints reached (%d)\n",
                           DEBUGGER_MAX_WATCHPOINTS);
                }
            }
        }
        /* ── WATCHPOINT DELETE ──────────────────────────── */
        else if (strcmp(cmd, "wd") == 0) {
            if (!arg1[0]) {
                printf("  Usage: wd <index>\n");
            } else {
                int idx = atoi(arg1);
                if (debugger_remove_watchpoint(dbg, idx)) {
                    printf("  Watchpoint #%d removed\n", idx);
                    debugger_install_watchpoint_trace(dbg, emu);
                } else {
                    printf("  Invalid watchpoint index\n");
                }
            }
        }
        /* ── ACCESS-MAP REGION (US 3): per-byte r/w/x breakpoints ── */
        else if (strcmp(cmd, "wr") == 0) {
            if (!arg1[0]) {
                /* List flagged runs (coalesce contiguous same-flag bytes). */
                if (debugger_amap_count() == 0) {
                    printf("  No access-map flags set\n");
                } else {
                    printf("  Access map (%u byte%s flagged):\n",
                           debugger_amap_count(), debugger_amap_count() > 1 ? "s" : "");
                    int shown = 0;
                    uint32_t a = 0;
                    while (a < 0x10000 && shown < 32) {
                        uint8_t f = debugger_amap_get((uint16_t)a);
                        if (!f) { a++; continue; }
                        uint32_t start = a;
                        while (a < 0x10000 && debugger_amap_get((uint16_t)a) == f) a++;
                        printf("    $%04X-$%04X  %c%c%c\n", (uint16_t)start, (uint16_t)(a - 1),
                               (f & AMAP_R) ? 'r' : '-', (f & AMAP_W) ? 'w' : '-',
                               (f & AMAP_X) ? 'x' : '-');
                        shown++;
                    }
                }
            } else if (strcasecmp(arg1, "clear") == 0) {
                debugger_amap_clear();
                debugger_install_watchpoint_trace(dbg, emu);
                printf("  Access map cleared\n");
            } else {
                uint16_t start, end;
                if (!arg2[0] || !parse_addr(emu, arg1, &start) ||
                    !parse_addr(emu, arg2, &end)) {
                    printf("  Usage: wr START END [rwx]   (default rw)  |  wr  |  wr clear\n");
                    return;
                }
                uint8_t flags = AMAP_R | AMAP_W;   /* default rw */
                char a3[16] = {0};
                if (sscanf(line, "%*s %*s %*s %15s", a3) == 1 &&
                    !debugger_amap_parse_flags(a3, &flags)) {
                    printf("  Bad flags '%s' (subset of rwx)\n", a3);
                    return;
                }
                uint32_t n = debugger_amap_set(start, end, flags);
                debugger_install_watchpoint_trace(dbg, emu);
                printf("  Flagged %u byte%s $%04X-$%04X as %c%c%c\n",
                       n, n > 1 ? "s" : "", start, end,
                       (flags & AMAP_R) ? 'r' : '-', (flags & AMAP_W) ? 'w' : '-',
                       (flags & AMAP_X) ? 'x' : '-');
            }
        }
        /* ── VIA STATE ──────────────────────────────────── */
        else if (strcmp(cmd, "via") == 0) {
            show_via_state(emu);
        }
        /* ── PSG STATE ──────────────────────────────────── */
        else if (strcmp(cmd, "psg") == 0) {
            show_psg_state(emu);
        }
        /* ── DISK / FDC STATE ──────────────────────────── */
        else if (strcmp(cmd, "disk") == 0 || strcmp(cmd, "fdc") == 0) {
            show_disk_state(emu);
        }
        /* ── ACIA 6551 STATE ───────────────────────────── */
        else if (strcmp(cmd, "acia") == 0 || strcmp(cmd, "serial") == 0) {
            show_acia_state(emu);
        }
        /* ── TAPE / CASSETTE STATE ─────────────────────── */
        else if (strcmp(cmd, "tape") == 0 || strcmp(cmd, "cassette") == 0) {
            show_tape_state(emu);
        }
        /* ── LOCI MIA STATE (sprint 34d3) ───────────────── */
        else if (strcmp(cmd, "loci") == 0) {
            show_loci_state(emu);
        }
        /* ── INSPECTION ÉLARGIE (US 5) ──────────────────── */
        else if (strcmp(cmd, "video") == 0 || strcmp(cmd, "ula") == 0) {
            show_video_state(emu);
        }
        else if (strcmp(cmd, "kbd") == 0 || strcmp(cmd, "keyboard") == 0) {
            show_keyboard_state(emu);
        }
        else if (strcmp(cmd, "joy") == 0 || strcmp(cmd, "joystick") == 0) {
            show_joystick_state(emu);
        }
        else if (strcmp(cmd, "printer") == 0 || strcmp(cmd, "mcp40") == 0) {
            show_printer_state(emu);
        }
        /* ── STACK ──────────────────────────────────────── */
        else if (strcmp(cmd, "stack") == 0) {
            show_stack(emu);
        }
        /* ── SET REGISTER ───────────────────────────────── */
        else if (strcmp(cmd, "set") == 0) {
            if (strcasecmp(arg1, "via") == 0) {
                /* set via <reg 0-15> <value> — write a VIA 6522 register */
                const char* p = skip_ws(skip_token(skip_ws(line)));  /* after "set" */
                p = skip_ws(skip_token(p));                          /* after "via" */
                p = skip_ws(skip_token(p));                          /* after reg → value */
                uint16_t regv = 0, valv = 0;
                if (!arg2[0] || !*p ||
                    !parse_addr(emu, arg2, &regv) || !parse_addr(emu, p, &valv) ||
                    regv > 15) {
                    printf("  Usage: set via <reg 0-15> <value>\n");
                } else {
                    via_write(&emu->via, (uint8_t)regv, (uint8_t)valv);
                    printf("  VIA[$%X] = $%02X\n", (unsigned)regv, (uint8_t)valv);
                }
            } else if (!arg1[0] || !arg2[0]) {
                printf("  Usage: set <reg> <value>  (reg: A,X,Y,SP,PC,P)\n");
                printf("         set via <reg 0-15> <value>\n");
            } else {
                uint16_t val = (uint16_t)strtol(arg2, NULL, 16);
                /* Case-insensitive register name */
                char reg[8];
                strncpy(reg, arg1, sizeof(reg) - 1);
                reg[sizeof(reg) - 1] = '\0';
                for (int i = 0; reg[i]; i++) reg[i] = (char)toupper(reg[i]);

                if (strcmp(reg, "A") == 0) {
                    emu->cpu.A = (uint8_t)val;
                    printf("  A = $%02X\n", emu->cpu.A);
                } else if (strcmp(reg, "X") == 0) {
                    emu->cpu.X = (uint8_t)val;
                    printf("  X = $%02X\n", emu->cpu.X);
                } else if (strcmp(reg, "Y") == 0) {
                    emu->cpu.Y = (uint8_t)val;
                    printf("  Y = $%02X\n", emu->cpu.Y);
                } else if (strcmp(reg, "SP") == 0) {
                    emu->cpu.SP = (uint8_t)val;
                    printf("  SP = $%02X\n", emu->cpu.SP);
                } else if (strcmp(reg, "PC") == 0) {
                    emu->cpu.PC = val;
                    printf("  PC = $%04X\n", emu->cpu.PC);
                } else if (strcmp(reg, "P") == 0) {
                    emu->cpu.P = (uint8_t)val;
                    printf("  P = $%02X\n", emu->cpu.P);
                } else {
                    printf("  Unknown register: %s\n", arg1);
                }
            }
        }
        /* ── HUNT (iterative memory search / cheat-finder) ─ */
        else if (strcmp(cmd, "hunt") == 0) {
            if (!arg1[0]) {
                debugger_hunt_start(emu);
                printf("  Hunt started: %u candidates (whole address space)\n",
                       hunt_count);
            } else if (strcasecmp(arg1, "list") == 0) {
                if (!hunt_active) {
                    printf("  No active hunt (type `hunt` to start)\n");
                } else {
                    printf("  %u candidate%s:\n", hunt_count,
                           hunt_count == 1 ? "" : "s");
                    int shown = 0;
                    for (uint32_t a = 0; a < 0x10000 && shown < 64; a++) {
                        if (!hunt_cand[a]) continue;
                        const char* s = symbol_lookup(&emu->symbols, (uint16_t)a);
                        printf("    $%04X = $%02X%s%s\n", (uint16_t)a,
                               dbg_peek(emu, (uint16_t)a), s ? "  " : "", s ? s : "");
                        shown++;
                    }
                    if (hunt_count > 64) printf("    ... (showing first 64)\n");
                }
            } else if (strcasecmp(arg1, "clear") == 0 ||
                       strcasecmp(arg1, "reset") == 0) {
                hunt_active = false;
                hunt_count = 0;
                printf("  Hunt cleared\n");
            } else if (!hunt_active) {
                printf("  No active hunt (type `hunt` to start)\n");
            } else {
                hunt_pred_t pred = HUNT_EQ;
                uint8_t val = 0;
                bool ok = true;
                if (strcmp(arg1, "+") == 0)                        pred = HUNT_GT;
                else if (strcmp(arg1, "-") == 0)                   pred = HUNT_LT;
                else if (strcmp(arg1, "!") == 0 ||
                         strcmp(arg1, "!=") == 0)                  pred = HUNT_CHANGED;
                else if (strcmp(arg1, "=") == 0 && !arg2[0])       pred = HUNT_UNCHANGED;
                else {
                    /* value form: "V", "=V", or "= V" */
                    const char* vs = (arg1[0] == '=') ? arg1 + 1 : arg1;
                    if (!*vs) vs = arg2;
                    uint16_t v16;
                    if (!parse_addr(emu, vs, &v16) || v16 > 0xFF) ok = false;
                    else { pred = HUNT_EQ; val = (uint8_t)v16; }
                }
                if (!ok) {
                    printf("  Usage: hunt [V | = | + | - | ! | list | clear]\n");
                } else {
                    uint32_t k = debugger_hunt_refine(emu, pred, val);
                    printf("  %u candidate%s remain\n", k, k == 1 ? "" : "s");
                }
            }
        }
        /* ── MEMORY → FILE / FILE → MEMORY ──────────────── */
        else if (strcmp(cmd, "save") == 0) {
            char fname[128] = {0}, a_addr[32] = {0}, a_len[32] = {0};
            if (sscanf(line, "%*s %127s %31s %31s", fname, a_addr, a_len) != 3) {
                printf("  Usage: save FILE addr len\n");
                return;
            }
            uint16_t addr, len16;
            if (!parse_addr(emu, a_addr, &addr)) {
                printf("  Bad address: %s\n", a_addr); return;
            }
            if (!parse_addr(emu, a_len, &len16)) {
                printf("  Bad length: %s\n", a_len); return;
            }
            if (debugger_save_region(emu, fname, addr, len16))
                printf("  Wrote %u byte(s) from $%04X to %s\n",
                       (unsigned)len16, addr, fname);
            else
                printf("  Error writing %s\n", fname);
        }
        else if (strcmp(cmd, "load") == 0) {
            char fname[128] = {0}, a_addr[32] = {0};
            if (sscanf(line, "%*s %127s %31s", fname, a_addr) != 2) {
                printf("  Usage: load FILE addr\n");
                return;
            }
            uint16_t addr;
            if (!parse_addr(emu, a_addr, &addr)) {
                printf("  Bad address: %s\n", a_addr); return;
            }
            long n = debugger_load_region(emu, fname, addr);
            if (n < 0) printf("  Error reading %s\n", fname);
            else printf("  Loaded %ld byte(s) into $%04X from %s\n", n, addr, fname);
        }
        else if (strcmp(cmd, "disf") == 0) {
            char fname[128] = {0}, a_addr[32] = {0}, a_n[32] = {0};
            if (sscanf(line, "%*s %127s %31s %31s", fname, a_addr, a_n) != 3) {
                printf("  Usage: disf FILE addr count\n");
                return;
            }
            uint16_t addr;
            if (!parse_addr(emu, a_addr, &addr)) {
                printf("  Bad address: %s\n", a_addr); return;
            }
            int count = atoi(a_n);
            if (count <= 0 || count > 4096) {
                printf("  Count must be 1..4096\n"); return;
            }
            FILE* f = fopen(fname, "w");
            if (!f) { printf("  Error writing %s\n", fname); return; }
            uint16_t a = addr;
            for (int i = 0; i < count; i++) {
                char dis[80];
                int len = cpu_disassemble(&emu->cpu, a, dis, sizeof(dis));
                fprintf(f, "$%04X: %s\n", a, dis);
                a = (uint16_t)(a + (len > 0 ? len : 1));
            }
            fclose(f);
            printf("  Disassembled %d instruction(s) from $%04X to %s\n",
                   count, addr, fname);
        }
        /* ── SAVE STATE / LOAD STATE (.ost) ─────────────── */
        else if (strcmp(cmd, "ss") == 0) {
            if (!arg1[0]) printf("  Usage: ss FILE   (save machine state)\n");
            else if (!savestate_save) printf("  Save state unavailable in this build\n");
            else if (savestate_save(emu, arg1)) printf("  Saved state to %s\n", arg1);
            else printf("  Error saving state to %s\n", arg1);
        }
        else if (strcmp(cmd, "sl") == 0) {
            if (!arg1[0]) printf("  Usage: sl FILE   (load machine state)\n");
            else if (!savestate_load) printf("  Load state unavailable in this build\n");
            else if (savestate_load(emu, arg1)) printf("  Loaded state from %s\n", arg1);
            else printf("  Error loading state from %s\n", arg1);
        }
        /* ── CONDITIONAL TRACE (Epic 6 / US 1) ──────────── */
        else if (strcmp(cmd, "trace") == 0) {
            cpu_trace_t* t = &emu->trace;
            const char* rest = skip_ws(skip_token(skip_ws(line)));  /* after "trace" */
            rest = skip_ws(skip_token(rest));                       /* after sub → spec */
            if (!arg1[0] || strcasecmp(arg1, "status") == 0) {
                printf("  trace: active=%d armed=%d count=%llu ring=%u/%u stop_hit=%d\n",
                       t->active, t->armed, (unsigned long long)t->count,
                       trace_ring_count(t), t->ring_cap, t->stop_hit);
            } else if (strcasecmp(arg1, "start") == 0) {
                trace_start_t sc; uint16_t spc; trace_stop_t stc; uint16_t sa;
                uint64_t scy; uint32_t ring; bool sym;
                if (!trace_parse_spec(rest, &sc, &spc, &stc, &sa, &scy, &ring, &sym)) {
                    printf("  Usage: trace start [now|pc:HEX] "
                           "[stop:cycle:N|stop:brk|stop:write:HEX|stop:read:HEX] "
                           "[ring:N] [sym]\n");
                } else {
                    trace_set_symbols(t, &emu->symbols);
                    trace_arm(t, sc, spc, stc, sa, scy, ring, sym);
                    trace_install_mem_hook(t, &emu->memory);
                    printf("  Trace armed (active=%d, ring=%u)\n", t->active, t->ring_cap);
                }
            } else if (strcasecmp(arg1, "stop") == 0) {
                trace_stop(t);
                printf("  Trace stopped (%llu instr, ring=%u)\n",
                       (unsigned long long)t->count, trace_ring_count(t));
            } else if (strcasecmp(arg1, "save") == 0) {
                if (!rest[0]) printf("  Usage: trace save <file>\n");
                else if (trace_save_ring(t, rest)) printf("  Saved %u instr to %s\n",
                                                          trace_ring_count(t), rest);
                else printf("  Nothing to save / write failed\n");
            } else if (strcasecmp(arg1, "off") == 0) {
                trace_reset(t);
                trace_install_mem_hook(t, &emu->memory);
                printf("  Trace off\n");
            } else {
                printf("  Unknown: trace %s (start|stop|save|status|off)\n", arg1);
            }
        }
        /* ── RAM STUCK-BIT FAULT INJECTION (US 6) ────────── */
        else if (strcmp(cmd, "stuck") == 0) {
            if (!arg1[0]) {
                printf("  RAM stuck bits: stuck0=$%02X (→0)  stuck1=$%02X (→1)\n",
                       emu->memory.stuck0, emu->memory.stuck1);
                printf("  Usage: stuck S0 [S1]   (S0=bits forced to 0, S1=forced to 1; 0 0 = off)\n");
            } else {
                uint16_t s0 = 0, s1 = 0;
                if (!parse_addr(emu, arg1, &s0) ||
                    (arg2[0] && !parse_addr(emu, arg2, &s1))) {
                    printf("  Usage: stuck S0 [S1]\n");
                } else {
                    memory_set_stuck_bits(&emu->memory, (uint8_t)s0, (uint8_t)s1);
                    printf("  RAM stuck bits set: stuck0=$%02X stuck1=$%02X\n",
                           (uint8_t)s0, (uint8_t)s1);
                }
            }
        }
        /* ── QUIT ───────────────────────────────────────── */
        else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
            emu->running = false;
            dbg->active = false;
        }
        /* ── HELP ───────────────────────────────────────── */
        else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
            show_help();
        }
        /* ── UNKNOWN ────────────────────────────────────── */
        else {
            printf("  Unknown command: '%s'. Type 'h' for help.\n", cmd);
        }
}

void debugger_repl(debugger_t* dbg, emulator_t* emu) {
    dbg->active = true;
    dbg->step_mode = false;

    /* Reset disasm pagination on every break — `d` first shows around PC,
     * subsequent `d` calls page forward, `d -` walks back through the
     * navigations done within this session. */
    dbg->disasm_cursor_valid = false;
    dbg->disasm_history_top = 0;

    /* Show current state on entry */
    printf("\n*** DEBUGGER BREAK at $%04X ***\n", emu->cpu.PC);
    show_registers(emu);
    show_disassembly(emu, emu->cpu.PC, 1);

    char line[256];
    while (dbg->active) {
        printf("dbg> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF on stdin - quit */
            emu->running = false;
            dbg->active = false;
            break;
        }

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines */
        if (len == 0)
            continue;

        process_repl_line(dbg, emu, line);
    }
}

/* Public wrapper: execute a single REPL command line. */
void debugger_repl_run_line(debugger_t* dbg, emulator_t* emu, const char* line) {
    if (!line || !*line) return;
    process_repl_line(dbg, emu, line);
}
