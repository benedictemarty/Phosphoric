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
#include "memory/memory.h"
#include "io/via6522.h"
#include "audio/audio.h"

/* Parse an address argument: tries the symbol table first (case-insensitive),
 * then falls back to hex parsing. Returns true if recognised. */
static bool parse_addr(const emulator_t* emu, const char* s, uint16_t* out) {
    if (!s || !*s) return false;
    if (symbol_resolve(&emu->symbols, s, out)) return true;
    if (*s == '$') s++;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 16);
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

static bool parse_condition(const emulator_t* emu, const char* text,
                            bp_condition_t* out) {
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
        if (!symbol_resolve(&emu->symbols, addr_buf, &addr)) {
            const char* s = addr_buf;
            if (*s == '$') s++;
            else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            char* end = NULL;
            unsigned long v = strtoul(s, &end, 16);
            if (end == s) return false;
            addr = (uint16_t)v;
        }
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

    /* RHS value: hex, decimal, or symbol */
    char rhs_buf[48];
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && n < sizeof(rhs_buf) - 1) {
        rhs_buf[n] = p[n]; n++;
    }
    rhs_buf[n] = '\0';
    if (n == 0) return false;
    uint16_t v16;
    if (symbol_resolve(&emu->symbols, rhs_buf, &v16)) {
        out->value = v16;
    } else {
        const char* s = rhs_buf;
        int base = 10;
        if (*s == '$') { s++; base = 16; }
        else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
        else if (strchr(s, 'A') || strchr(s, 'B') || strchr(s, 'C') ||
                 strchr(s, 'D') || strchr(s, 'E') || strchr(s, 'F') ||
                 strchr(s, 'a') || strchr(s, 'b') || strchr(s, 'c') ||
                 strchr(s, 'd') || strchr(s, 'e') || strchr(s, 'f')) base = 16;
        char* end = NULL;
        unsigned long v = strtoul(s, &end, base);
        if (end == s) return false;
        out->value = (uint32_t)v;
    }
    return true;
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
        if (eval_cond(&dbg->breakpoints[i].cond, emu)) return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  WATCHPOINT MANAGEMENT                                              */
/* ═══════════════════════════════════════════════════════════════════ */

int debugger_add_watchpoint(debugger_t* dbg, uint16_t addr) {
    if (dbg->num_watchpoints >= DEBUGGER_MAX_WATCHPOINTS)
        return -1;
    /* Check for duplicate */
    for (int i = 0; i < dbg->num_watchpoints; i++) {
        if (dbg->watchpoints[i] == addr)
            return i;
    }
    dbg->watchpoints[dbg->num_watchpoints] = addr;
    return dbg->num_watchpoints++;
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
/*  MEMORY TRACE CALLBACK (for watchpoints)                            */
/* ═══════════════════════════════════════════════════════════════════ */

/* Global pointer to active debugger (needed by trace callback) */
static debugger_t* g_trace_debugger = NULL;

static void watchpoint_trace_callback(uint16_t address, uint8_t value, mem_access_type_t type) {
    (void)value;
    if (type != MEM_WRITE || !g_trace_debugger)
        return;
    for (int i = 0; i < g_trace_debugger->num_watchpoints; i++) {
        if (g_trace_debugger->watchpoints[i] == address) {
            g_trace_debugger->watch_triggered = true;
            g_trace_debugger->watch_addr_hit = address;
            return;
        }
    }
}

void debugger_install_watchpoint_trace(debugger_t* dbg, emulator_t* emu) {
    g_trace_debugger = dbg;
    if (dbg->num_watchpoints > 0) {
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

    /* Step mode: always break */
    if (dbg->step_mode)
        return true;

    /* Temporary breakpoint (step-over) */
    if (dbg->has_temp_breakpoint && pc == dbg->temp_breakpoint) {
        dbg->has_temp_breakpoint = false;
        return true;
    }

    /* PC breakpoint hit (evaluates condition if present) */
    (void)pc;
    if (debugger_check_pc(dbg, emu))
        return true;

    /* Watchpoint triggered */
    if (dbg->watch_triggered) {
        printf("\n*** WATCHPOINT hit: write to $%04X ***\n", dbg->watch_addr_hit);
        dbg->watch_triggered = false;
        return true;
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
        printf(" %s", buf);
        if (addr == emu->cpu.PC)
            printf("  <---");
        printf("\n");
        addr = (uint16_t)(addr + bytes);
    }
    return addr;
}

static void show_memory_dump(emulator_t* emu, uint16_t addr, int len) {
    for (int offset = 0; offset < len; offset += 16) {
        printf("  $%04X: ", (uint16_t)(addr + offset));
        /* Hex */
        for (int i = 0; i < 16 && (offset + i) < len; i++) {
            printf("%02X ", memory_read(&emu->memory, (uint16_t)(addr + offset + i)));
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
            uint8_t c = memory_read(&emu->memory, (uint16_t)(addr + offset + i));
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

static void show_help(void) {
    printf("\n  Debugger Commands:\n");
    printf("  ─────────────────────────────────────────────────\n");
    printf("  s / step          Step 1 instruction\n");
    printf("  n / next          Step over (JSR → break at return)\n");
    printf("  c / continue      Continue execution\n");
    printf("  r / regs          Show CPU registers\n");
    printf("  d                 Disassemble next page (page size persists)\n");
    printf("  d addr [n]        Jump-disasm; push current page to history\n");
    printf("  d +               Same as `d` (next page)\n");
    printf("  d -               Pop history (previous page)\n");
    printf("  m addr [len]      Memory dump hex+ASCII (default: 256)\n");
    printf("  m addr = V1 [V2...]  Write byte(s) to memory\n");
    printf("  b addr            Add PC breakpoint\n");
    printf("  b addr if EXPR    Conditional breakpoint\n");
    printf("                    EXPR: REG op VAL | M[ADDR] op VAL\n");
    printf("                    REG: A X Y SP P PC   op: == != < <= > >=\n");
    printf("  b                 List all breakpoints\n");
    printf("  bd n              Delete breakpoint #n\n");
    printf("  w addr            Add write watchpoint\n");
    printf("  w                 List all watchpoints\n");
    printf("  wd n              Delete watchpoint #n\n");
    printf("  via               Show VIA 6522 state\n");
    printf("  psg               Show PSG AY-3-8910 state\n");
    printf("  stack             Show stack contents\n");
    printf("  set reg val       Set register (A,X,Y,SP,PC,P)\n");
    printf("  sym [name|addr]   List symbols / resolve name or address\n");
    printf("  (addr args accept symbol names if --symbols was loaded)\n");
    printf("  q / quit          Quit emulator\n");
    printf("  h / help          Show this help\n");
    printf("\n");
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
            dbg->step_mode = true;
            dbg->active = false;
            /* Execute one instruction and come back */
        }
        /* ── NEXT (step-over) ───────────────────────────── */
        else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
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
            if (!arg1[0]) {
                if (emu->symbols.count == 0) {
                    printf("  No symbols loaded (use --symbols FILE)\n");
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
                printf("  Usage: m addr [len]          dump memory\n");
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
                int len2 = 256;
                if (arg2[0])
                    len2 = (int)strtol(arg2, NULL, 0);
                if (len2 < 1) len2 = 1;
                if (len2 > 65536) len2 = 65536;
                show_memory_dump(emu, addr, len2);
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
                    bp_condition_t cond;
                    if (!parse_condition(emu, if_pos, &cond)) {
                        debugger_remove_breakpoint(dbg, idx);
                        printf("  Invalid condition: %s\n", if_pos);
                        printf("  Syntax: REG op VALUE  or  M[ADDR] op VALUE\n");
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
        /* ── WATCHPOINT ─────────────────────────────────── */
        else if (strcmp(cmd, "w") == 0) {
            if (!arg1[0]) {
                /* List watchpoints */
                if (dbg->num_watchpoints == 0) {
                    printf("  No watchpoints set\n");
                } else {
                    printf("  Watchpoints (write):\n");
                    for (int i = 0; i < dbg->num_watchpoints; i++) {
                        const char* s = symbol_lookup(&emu->symbols, dbg->watchpoints[i]);
                        if (s) printf("    #%d: $%04X  %s\n", i, dbg->watchpoints[i], s);
                        else   printf("    #%d: $%04X\n", i, dbg->watchpoints[i]);
                    }
                }
            } else {
                uint16_t addr;
                if (!parse_addr(emu, arg1, &addr)) {
                    printf("  Unknown address/symbol: %s\n", arg1);
                    return;
                }
                int idx = debugger_add_watchpoint(dbg, addr);
                if (idx >= 0) {
                    printf("  Watchpoint #%d set at $%04X (write)\n", idx, addr);
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
        /* ── VIA STATE ──────────────────────────────────── */
        else if (strcmp(cmd, "via") == 0) {
            show_via_state(emu);
        }
        /* ── PSG STATE ──────────────────────────────────── */
        else if (strcmp(cmd, "psg") == 0) {
            show_psg_state(emu);
        }
        /* ── STACK ──────────────────────────────────────── */
        else if (strcmp(cmd, "stack") == 0) {
            show_stack(emu);
        }
        /* ── SET REGISTER ───────────────────────────────── */
        else if (strcmp(cmd, "set") == 0) {
            if (!arg1[0] || !arg2[0]) {
                printf("  Usage: set <reg> <value>  (reg: A,X,Y,SP,PC,P)\n");
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
