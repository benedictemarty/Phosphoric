/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file debugger_repl.c
 * @brief Débogueur : boucle de commandes (REPL)
 * @author bmarty <bmarty@mailo.com>
 *
 * Découpé de src/debugger.c (sprint F du plan d'architecture), sans changement
 * de comportement ; symboles partagés : include/debugger_internal.h.
 *
 * Une commande = un gestionnaire repl_*() ; k_repl_cmds[] associe chaque nom
 * (et ses alias) à son gestionnaire. Ajouter une commande : écrire le
 * gestionnaire, puis l'inscrire dans la table.
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
/*  REPL COMMAND LOOP                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

/* Arguments d'une ligne de commande : les trois premiers mots, la ligne
 * entière (les commandes à plus de deux arguments la relisent) et le contexte. */
typedef struct {
    debugger_t* dbg;
    emulator_t* emu;
    const char* line;
    char cmd[32];
    char arg1[32];
    char arg2[32];
} repl_args_t;

/* ── STEP ───────────────────────────────────────── */
static void repl_step(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    dbg_undo_push(dbg, emu);   /* sprint 34d5 P2-F */
    dbg->step_mode = true;
    dbg->active = false;
    /* Execute one instruction and come back */
}

/* ── NEXT (step-over) ───────────────────────────── */
static void repl_next(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    dbg_undo_push(dbg, emu);   /* sprint 34d5 P2-F */
    /* Check if current instruction is JSR ($20) */
    uint8_t opcode = memory_peek(&emu->memory, emu->cpu.PC);
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
static void repl_undo(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    if (dbg_undo_pop(dbg, emu)) {
        printf("  Rewound 1 step. PC=$%04X cycles=%llu "
               "(%u snapshots left)\n",
               emu->cpu.PC, (unsigned long long)emu->cpu.cycles,
               dbg->undo_count);
        dbg_show_disassembly(emu, emu->cpu.PC, 1);
    } else {
        printf("  Nothing to undo (ring empty)\n");
    }
}

/* ── CONTINUE ───────────────────────────────────── */
static void repl_continue(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    dbg->step_mode = false;
    dbg->active = false;
}

/* ── REGISTERS ──────────────────────────────────── */
static void repl_regs(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_registers(emu);
}

/* ── SYMBOLS ────────────────────────────────────── */
static void repl_symbols(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
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
        if (dbg_parse_addr(emu, arg1, &addr)) {
            const char* s = symbol_lookup(&emu->symbols, addr);
            if (s) printf("  $%04X = %s\n", addr, s);
            else   printf("  $%04X = (no symbol)\n", addr);
        } else {
            printf("  Unknown symbol: %s\n", arg1);
        }
    }
}

/* ── DISASSEMBLE (paginated) ────────────────────── */
static void repl_disasm(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
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
        if (dbg_parse_addr(emu, arg1, &a)) {
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

    uint16_t next = dbg_show_disassembly(emu, addr, count);
    dbg->disasm_cursor = next;
    dbg->disasm_cursor_valid = true;
}

/* ── MEMORY DUMP / WRITE ────────────────────────── */
static void repl_memory(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
    if (!arg1[0]) {
        printf("  Usage: m addr [len] [bank]   dump memory (bank: cpu|ram|rom|overlay)\n");
        printf("         m addr = V1 [V2 ...]  write byte(s)\n");
        return;
    }
    uint16_t addr;
    if (!dbg_parse_addr(emu, arg1, &addr)) {
        printf("  Unknown address/symbol: %s\n", arg1);
        return;
    }
    /* Detect "=" in arg2 (separator) or as part of arg2 like "=42".
     * Anything after = is a list of values; we re-scan `line` after
     * the '=' character to read more than the 3 tokens sscanf got. */
    const char* eq = strchr(line, '=');
    if (eq) {
        const char* vals = dbg_skip_ws(eq + 1);
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
            if (tn == 0) { vals = dbg_skip_ws(vals); continue; }
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
            vals = dbg_skip_ws(vals);
            while (*vals == ',') vals = dbg_skip_ws(vals + 1);
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
        dbg_show_memory_dump(emu, addr, len2, bank);
    }
}

/* ── MEMORY SEARCH ──────────────────────────────── */
static void repl_find(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    const char* p = dbg_skip_ws(dbg_skip_token(dbg_skip_ws(line)));  /* after "find" */
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
            p = dbg_skip_ws(p);
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
static void repl_assemble(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
    if (!arg1[0] || !arg2[0]) {
        printf("  Usage: a addr MNEMONIC [operand]\n");
        printf("         e.g. a 0400 LDA #$41   a 0402 STA $BB80   a 0405 BNE 0400\n");
        return;
    }
    uint16_t addr;
    if (!dbg_parse_addr(emu, arg1, &addr)) {
        printf("  Unknown address/symbol: %s\n", arg1);
        return;
    }
    /* Operand = raw text following the mnemonic token (arg2 holds the
     * mnemonic). Walk past cmd, addr and mnemonic in `line`. */
    const char* p = dbg_skip_ws(dbg_skip_token(dbg_skip_ws(line)));   /* after cmd */
    p = dbg_skip_ws(dbg_skip_token(p));                            /* after addr */
    p = dbg_skip_ws(dbg_skip_token(p));                            /* after mnemonic */
    int n = dbg_assemble_one(emu, addr, arg2, p);
    if (n > 0) {
        char dis[80];
        cpu_disassemble(&emu->cpu, addr, dis, sizeof(dis));
        printf("  $%04X: %s\n", addr, dis);
        dbg->disasm_cursor = (uint16_t)(addr + n);
        dbg->disasm_cursor_valid = true;
    }
}

/* ── BREAKPOINT ─────────────────────────────────── */
static void repl_breakpoint(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
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
        if (!dbg_parse_addr(emu, arg1, &addr)) {
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
            if (p) if_pos = dbg_skip_ws(p + 4);
        }
        int idx = debugger_add_breakpoint(dbg, addr);
        if (idx < 0) {
            printf("  Error: maximum breakpoints reached (%d)\n",
                   DEBUGGER_MAX_BREAKPOINTS);
            return;
        }
        if (if_pos && *if_pos) {
            bp_condexpr_t cond;
            if (!dbg_parse_condexpr(emu, if_pos, &cond)) {
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
static void repl_breakpoint_delete(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    char* arg1 = ra->arg1;
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
static void repl_raster(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    char* arg1 = ra->arg1;
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

static void repl_raster_delete(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    char* arg1 = ra->arg1;
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
static void repl_watchpoint(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
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
                       dbg_watch_mode_name(w->mode), s ? "  " : "", s ? s : "");
            }
        }
    } else {
        uint16_t addr;
        if (!dbg_parse_addr(emu, arg1, &addr)) {
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
                   idx, addr, dbg_watch_mode_name(mode));
            debugger_install_watchpoint_trace(dbg, emu);
        } else {
            printf("  Error: maximum watchpoints reached (%d)\n",
                   DEBUGGER_MAX_WATCHPOINTS);
        }
    }
}

/* ── WATCHPOINT DELETE ──────────────────────────── */
static void repl_watchpoint_delete(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    char* arg1 = ra->arg1;
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
static void repl_access_region(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
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
        if (!arg2[0] || !dbg_parse_addr(emu, arg1, &start) ||
            !dbg_parse_addr(emu, arg2, &end)) {
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
static void repl_via(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_via_state(emu);
}

/* ── PSG STATE ──────────────────────────────────── */
static void repl_psg(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_psg_state(emu);
}

/* ── DISK / FDC STATE ──────────────────────────── */
static void repl_disk(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_disk_state(emu);
}

/* ── ACIA 6551 STATE ───────────────────────────── */
static void repl_acia(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_acia_state(emu);
}

/* ── TAPE / CASSETTE STATE ─────────────────────── */
static void repl_tape(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_tape_state(emu);
}

/* ── LOCI MIA STATE (sprint 34d3) ───────────────── */
static void repl_loci(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_loci_state(emu);
}

/* ── INSPECTION ÉLARGIE (US 5) ──────────────────── */
static void repl_video(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_video_state(emu);
}

static void repl_keyboard(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_keyboard_state(emu);
}

static void repl_joystick(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_joystick_state(emu);
}

static void repl_printer(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_printer_state(emu);
}

/* ── STACK ──────────────────────────────────────── */
static void repl_stack(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    dbg_show_stack(emu);
}

/* ── SET REGISTER ───────────────────────────────── */
static void repl_set_register(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
    if (strcasecmp(arg1, "via") == 0) {
        /* set via <reg 0-15> <value> — write a VIA 6522 register */
        const char* p = dbg_skip_ws(dbg_skip_token(dbg_skip_ws(line)));  /* after "set" */
        p = dbg_skip_ws(dbg_skip_token(p));                          /* after "via" */
        p = dbg_skip_ws(dbg_skip_token(p));                          /* after reg → value */
        uint16_t regv = 0, valv = 0;
        if (!arg2[0] || !*p ||
            !dbg_parse_addr(emu, arg2, &regv) || !dbg_parse_addr(emu, p, &valv) ||
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
static void repl_hunt(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
    if (!arg1[0]) {
        debugger_hunt_start(emu);
        printf("  Hunt started: %u candidates (whole address space)\n",
               dbg_hunt_count);
    } else if (strcasecmp(arg1, "list") == 0) {
        if (!dbg_hunt_active) {
            printf("  No active hunt (type `hunt` to start)\n");
        } else {
            printf("  %u candidate%s:\n", dbg_hunt_count,
                   dbg_hunt_count == 1 ? "" : "s");
            int shown = 0;
            for (uint32_t a = 0; a < 0x10000 && shown < 64; a++) {
                if (!dbg_hunt_cand[a]) continue;
                const char* s = symbol_lookup(&emu->symbols, (uint16_t)a);
                printf("    $%04X = $%02X%s%s\n", (uint16_t)a,
                       dbg_peek(emu, (uint16_t)a), s ? "  " : "", s ? s : "");
                shown++;
            }
            if (dbg_hunt_count > 64) printf("    ... (showing first 64)\n");
        }
    } else if (strcasecmp(arg1, "clear") == 0 ||
               strcasecmp(arg1, "reset") == 0) {
        dbg_hunt_active = false;
        dbg_hunt_count = 0;
        printf("  Hunt cleared\n");
    } else if (!dbg_hunt_active) {
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
            if (!dbg_parse_addr(emu, vs, &v16) || v16 > 0xFF) ok = false;
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
static void repl_save_mem(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char fname[128] = {0}, a_addr[32] = {0}, a_len[32] = {0};
    if (sscanf(line, "%*s %127s %31s %31s", fname, a_addr, a_len) != 3) {
        printf("  Usage: save FILE addr len\n");
        return;
    }
    uint16_t addr, len16;
    if (!dbg_parse_addr(emu, a_addr, &addr)) {
        printf("  Bad address: %s\n", a_addr); return;
    }
    if (!dbg_parse_addr(emu, a_len, &len16)) {
        printf("  Bad length: %s\n", a_len); return;
    }
    if (debugger_save_region(emu, fname, addr, len16))
        printf("  Wrote %u byte(s) from $%04X to %s\n",
               (unsigned)len16, addr, fname);
    else
        printf("  Error writing %s\n", fname);
}

static void repl_load_mem(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char fname[128] = {0}, a_addr[32] = {0};
    if (sscanf(line, "%*s %127s %31s", fname, a_addr) != 2) {
        printf("  Usage: load FILE addr\n");
        return;
    }
    uint16_t addr;
    if (!dbg_parse_addr(emu, a_addr, &addr)) {
        printf("  Bad address: %s\n", a_addr); return;
    }
    long n = debugger_load_region(emu, fname, addr);
    if (n < 0) printf("  Error reading %s\n", fname);
    else printf("  Loaded %ld byte(s) into $%04X from %s\n", n, addr, fname);
}

static void repl_disasm_file(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char fname[128] = {0}, a_addr[32] = {0}, a_n[32] = {0};
    if (sscanf(line, "%*s %127s %31s %31s", fname, a_addr, a_n) != 3) {
        printf("  Usage: disf FILE addr count\n");
        return;
    }
    uint16_t addr;
    if (!dbg_parse_addr(emu, a_addr, &addr)) {
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
static void repl_state_save(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    char* arg1 = ra->arg1;
    if (!arg1[0]) printf("  Usage: ss FILE   (save machine state)\n");
    else if (!savestate_save) printf("  Save state unavailable in this build\n");
    else if (savestate_save(emu, arg1)) printf("  Saved state to %s\n", arg1);
    else printf("  Error saving state to %s\n", arg1);
}

static void repl_state_load(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    char* arg1 = ra->arg1;
    if (!arg1[0]) printf("  Usage: sl FILE   (load machine state)\n");
    else if (!savestate_load) printf("  Load state unavailable in this build\n");
    else if (savestate_load(emu, arg1)) printf("  Loaded state from %s\n", arg1);
    else printf("  Error loading state from %s\n", arg1);
}

/* ── CONDITIONAL TRACE (Epic 6 / US 1) ──────────── */
static void repl_trace(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    const char* line = ra->line;
    char* arg1 = ra->arg1;
    cpu_trace_t* t = &emu->trace;
    const char* rest = dbg_skip_ws(dbg_skip_token(dbg_skip_ws(line)));  /* after "trace" */
    rest = dbg_skip_ws(dbg_skip_token(rest));                       /* after sub → spec */
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
static void repl_stuck(repl_args_t* ra) {
    emulator_t* emu = ra->emu;
    char* arg1 = ra->arg1;
    char* arg2 = ra->arg2;
    if (!arg1[0]) {
        printf("  RAM stuck bits: stuck0=$%02X (→0)  stuck1=$%02X (→1)\n",
               emu->memory.stuck0, emu->memory.stuck1);
        printf("  Usage: stuck S0 [S1]   (S0=bits forced to 0, S1=forced to 1; 0 0 = off)\n");
    } else {
        uint16_t s0 = 0, s1 = 0;
        if (!dbg_parse_addr(emu, arg1, &s0) ||
            (arg2[0] && !dbg_parse_addr(emu, arg2, &s1))) {
            printf("  Usage: stuck S0 [S1]\n");
        } else {
            memory_set_stuck_bits(&emu->memory, (uint8_t)s0, (uint8_t)s1);
            printf("  RAM stuck bits set: stuck0=$%02X stuck1=$%02X\n",
                   (uint8_t)s0, (uint8_t)s1);
        }
    }
}

/* ── QUIT ───────────────────────────────────────── */
static void repl_quit(repl_args_t* ra) {
    debugger_t* dbg = ra->dbg;
    emulator_t* emu = ra->emu;
    emu->running = false;
    dbg->active = false;
}

/* ── HELP ───────────────────────────────────────── */
static void repl_help(repl_args_t* ra) {
    (void)ra;
    dbg_show_help();
}

/* ── UNKNOWN ────────────────────────────────────── */
static void repl_unknown(repl_args_t* ra) {
    const char* cmd = ra->cmd;
    printf("  Unknown command: '%s'. Type 'h' for help.\n", cmd);
}

/* Table de dispatch : un nom (ou alias) → son gestionnaire. */
static const struct {
    const char* name;
    void (*fn)(repl_args_t* ra);
} k_repl_cmds[] = {
    { "s", repl_step },
    { "step", repl_step },
    { "n", repl_next },
    { "next", repl_next },
    { "u", repl_undo },
    { "undo", repl_undo },
    { "c", repl_continue },
    { "continue", repl_continue },
    { "r", repl_regs },
    { "regs", repl_regs },
    { "sym", repl_symbols },
    { "d", repl_disasm },
    { "m", repl_memory },
    { "find", repl_find },
    { "a", repl_assemble },
    { "b", repl_breakpoint },
    { "bd", repl_breakpoint_delete },
    { "br", repl_raster },
    { "brd", repl_raster_delete },
    { "w", repl_watchpoint },
    { "wd", repl_watchpoint_delete },
    { "wr", repl_access_region },
    { "via", repl_via },
    { "psg", repl_psg },
    { "disk", repl_disk },
    { "fdc", repl_disk },
    { "acia", repl_acia },
    { "serial", repl_acia },
    { "tape", repl_tape },
    { "cassette", repl_tape },
    { "loci", repl_loci },
    { "video", repl_video },
    { "ula", repl_video },
    { "kbd", repl_keyboard },
    { "keyboard", repl_keyboard },
    { "joy", repl_joystick },
    { "joystick", repl_joystick },
    { "printer", repl_printer },
    { "mcp40", repl_printer },
    { "stack", repl_stack },
    { "set", repl_set_register },
    { "hunt", repl_hunt },
    { "save", repl_save_mem },
    { "load", repl_load_mem },
    { "disf", repl_disasm_file },
    { "ss", repl_state_save },
    { "sl", repl_state_load },
    { "trace", repl_trace },
    { "stuck", repl_stuck },
    { "q", repl_quit },
    { "quit", repl_quit },
    { "h", repl_help },
    { "help", repl_help },
};

/* Single-line REPL command dispatch. Used by the interactive REPL
 * loop and by the TUI's ':' command-line mode. */
static void process_repl_line(debugger_t* dbg, emulator_t* emu, const char* line) {
    repl_args_t a = { .dbg = dbg, .emu = emu, .line = line };
    /* Parse command */
    sscanf(line, "%31s %31s %31s", a.cmd, a.arg1, a.arg2);

    for (size_t i = 0; i < sizeof(k_repl_cmds) / sizeof(k_repl_cmds[0]); i++) {
        if (strcmp(a.cmd, k_repl_cmds[i].name) == 0) {
            k_repl_cmds[i].fn(&a);
            return;
        }
    }
    repl_unknown(&a);
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
    dbg_show_registers(emu);
    dbg_show_disassembly(emu, emu->cpu.PC, 1);

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

