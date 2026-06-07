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
    dbg->last_raster_line = -1;
    for (int i = 0; i < 8; i++) dbg->raster_bps[i] = -1;
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
                    printf("\n*** RASTER BREAK at line %d (frame_cyc=%d) ***\n",
                           bp, emu->frame_cycles);
                    dbg->last_raster_line = (int16_t)cur;
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
    printf("  br line           Raster bp at PAL line (0..311)\n");
    printf("  br                List raster breakpoints\n");
    printf("  brd n             Delete raster bp #n (or `brd *` to clear all)\n");
    printf("  w addr            Add write watchpoint\n");
    printf("  w                 List all watchpoints\n");
    printf("  wd n              Delete watchpoint #n\n");
    printf("  via               Show VIA 6522 state\n");
    printf("  psg               Show PSG AY-3-8910 state\n");
    printf("  disk / fdc        Show Microdisc WD1793 + 4 drives\n");
    printf("  acia / serial     Show ACIA 6551 registers + signals + FIFO\n");
    printf("  tape / cassette   Show tape position, status, next bytes\n");
    printf("  loci              Show LOCI MIA full state (regs, fds, mounts, DSK/TAP)\n");
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
