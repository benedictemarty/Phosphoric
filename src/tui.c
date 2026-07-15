/**
 * @file tui.c
 * @brief ncurses-based multi-pane debugger
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Layout (80x24 minimum, scales up):
 *   ┌──Registers──────────┬──Stack──────────────┐
 *   │ A:XX X:XX Y:XX ...  │ SP:XX -> top-of-stk │
 *   │ flags expansion     │ ...                 │
 *   ├──Disassembly────────┴─────────────────────┤
 *   │ $XXXX: LDA #$XX  ; SYMBOL              <- │
 *   │ ...                                       │
 *   ├──Memory─────────────┬──Breakpoints/Watch──┤
 *   │ $XXXX: ...          │ #0 $XXXX            │
 *   │ ...                 │ ...                 │
 *   ├───────────────────────────────────────────┤
 *   │ s step  n next  c continue  q quit  : cmd │
 *   └───────────────────────────────────────────┘
 */

#ifdef HAS_TUI

#include "tui.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "utils/symbols.h"
#include "debugger.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static WINDOW* w_regs   = NULL;
static WINDOW* w_stack  = NULL;
static WINDOW* w_disasm = NULL;
static WINDOW* w_mem    = NULL;
static WINDOW* w_bps    = NULL;
static WINDOW* w_status = NULL;

/* User-adjustable view state */
static uint16_t mem_view_addr = 0x0000;
static int      mem_view_rows = 0;        /* recomputed each draw */

static bool tui_active = false;

bool tui_init(void) {
    if (!initscr()) return false;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_CYAN,    COLOR_BLACK);  /* headers */
        init_pair(2, COLOR_GREEN,   COLOR_BLACK);  /* PC line */
        init_pair(3, COLOR_YELLOW,  COLOR_BLACK);  /* symbols */
        init_pair(4, COLOR_MAGENTA, COLOR_BLACK);  /* breakpoint */
    }
    tui_active = true;
    return true;
}

void tui_cleanup(void) {
    if (!tui_active) return;
    if (w_regs)   { delwin(w_regs);   w_regs   = NULL; }
    if (w_stack)  { delwin(w_stack);  w_stack  = NULL; }
    if (w_disasm) { delwin(w_disasm); w_disasm = NULL; }
    if (w_mem)    { delwin(w_mem);    w_mem    = NULL; }
    if (w_bps)    { delwin(w_bps);    w_bps    = NULL; }
    if (w_status) { delwin(w_status); w_status = NULL; }
    endwin();
    tui_active = false;
}

/* ───── Window layout ──────────────────────────────────────────── */

static void layout_windows(void) {
    int H, W;
    getmaxyx(stdscr, H, W);
    if (H < 20) H = 20;
    if (W < 80) W = 80;

    int top_h    = 6;
    int dis_h    = (H - top_h - 1) / 2;
    int bot_h    = H - top_h - dis_h - 1;
    int left_w   = W / 2;

    if (w_regs)   delwin(w_regs);
    if (w_stack)  delwin(w_stack);
    if (w_disasm) delwin(w_disasm);
    if (w_mem)    delwin(w_mem);
    if (w_bps)    delwin(w_bps);
    if (w_status) delwin(w_status);

    w_regs   = newwin(top_h, left_w,         0,      0);
    w_stack  = newwin(top_h, W - left_w,     0,      left_w);
    w_disasm = newwin(dis_h, W,              top_h,  0);
    w_mem    = newwin(bot_h, left_w,         top_h + dis_h, 0);
    w_bps    = newwin(bot_h, W - left_w,     top_h + dis_h, left_w);
    w_status = newwin(1,     W,              H - 1,  0);

    mem_view_rows = bot_h - 2;
}

/* ───── Panes ──────────────────────────────────────────────────── */

static void draw_box(WINDOW* w, const char* title) {
    box(w, 0, 0);
    if (title) {
        wattron(w, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(w, 0, 2, " %s ", title);
        wattroff(w, COLOR_PAIR(1) | A_BOLD);
    }
}

static void draw_regs(emulator_t* emu) {
    werase(w_regs);
    draw_box(w_regs, "Registers");
    const cpu6502_t* cpu = &emu->cpu;
    mvwprintw(w_regs, 1, 2, "PC:%04X  A:%02X  X:%02X  Y:%02X",
              cpu->PC, cpu->A, cpu->X, cpu->Y);
    mvwprintw(w_regs, 2, 2, "SP:%02X   P:%02X  CYC:%llu",
              cpu->SP, cpu->P, (unsigned long long)cpu->cycles);
    char flg[10];
    snprintf(flg, sizeof(flg), "%c%c-%c%c%c%c%c",
        (cpu->P & 0x80) ? 'N' : '.',
        (cpu->P & 0x40) ? 'V' : '.',
        (cpu->P & 0x10) ? 'B' : '.',
        (cpu->P & 0x08) ? 'D' : '.',
        (cpu->P & 0x04) ? 'I' : '.',
        (cpu->P & 0x02) ? 'Z' : '.',
        (cpu->P & 0x01) ? 'C' : '.');
    mvwprintw(w_regs, 3, 2, "Flags: %s", flg);
    const char* sym = symbol_lookup(&emu->symbols, cpu->PC);
    if (sym) {
        wattron(w_regs, COLOR_PAIR(3) | A_BOLD);
        mvwprintw(w_regs, 4, 2, "@ %s", sym);
        wattroff(w_regs, COLOR_PAIR(3) | A_BOLD);
    }
    wnoutrefresh(w_regs);
}

static void draw_stack(emulator_t* emu) {
    werase(w_stack);
    draw_box(w_stack, "Stack");
    uint8_t sp = emu->cpu.SP;
    int row = 1;
    int max_h, max_w;
    getmaxyx(w_stack, max_h, max_w); (void)max_w;
    for (int i = 0; i < max_h - 2 && i < 16; i++) {
        uint16_t addr = (uint16_t)(0x0100 + sp + 1 + i);
        if (addr > 0x01FF) break;
        uint8_t v = memory_read(&emu->memory, addr);
        mvwprintw(w_stack, row++, 2, "$%04X: %02X", addr, v);
    }
    if (row == 1)
        mvwprintw(w_stack, 1, 2, "(empty)");
    wnoutrefresh(w_stack);
}

static void draw_disasm(emulator_t* emu) {
    werase(w_disasm);
    draw_box(w_disasm, "Disassembly");
    int max_h, max_w;
    getmaxyx(w_disasm, max_h, max_w); (void)max_w;
    uint16_t addr = emu->cpu.PC;
    for (int i = 0; i < max_h - 2; i++) {
        char buf[64];
        int bytes = cpu_disassemble(&emu->cpu, addr, buf, sizeof(buf));
        const char* sym = symbol_lookup(&emu->symbols, addr);
        bool is_pc = (addr == emu->cpu.PC);
        bool is_bp = false;
        for (int b = 0; b < emu->debugger.num_breakpoints; b++) {
            if (emu->debugger.breakpoints[b].addr == addr) { is_bp = true; break; }
        }
        int row = i + 1;
        if (is_pc) wattron(w_disasm, COLOR_PAIR(2) | A_BOLD);
        else if (is_bp) wattron(w_disasm, COLOR_PAIR(4));
        char prefix = is_pc ? '>' : (is_bp ? '*' : ' ');
        uint8_t opc = memory_read(&emu->memory, addr);
        uint8_t cyc = cpu_opcode_cycles(opc);
        if (sym) {
            mvwprintw(w_disasm, row, 2, "%c $%04X: %-18s [%u] ; %s",
                      prefix, addr, buf, cyc, sym);
        } else {
            mvwprintw(w_disasm, row, 2, "%c $%04X: %-18s [%u]",
                      prefix, addr, buf, cyc);
        }
        if (is_pc) wattroff(w_disasm, COLOR_PAIR(2) | A_BOLD);
        else if (is_bp) wattroff(w_disasm, COLOR_PAIR(4));
        addr = (uint16_t)(addr + bytes);
    }
    wnoutrefresh(w_disasm);
}

static void draw_mem(emulator_t* emu) {
    werase(w_mem);
    char title[48];
    snprintf(title, sizeof(title), "Memory $%04X (PgUp/PgDn, g jump)",
             mem_view_addr);
    draw_box(w_mem, title);
    int max_h, max_w;
    getmaxyx(w_mem, max_h, max_w); (void)max_w;
    int rows = max_h - 2;
    int bpr  = 8;
    for (int r = 0; r < rows; r++) {
        uint16_t a = (uint16_t)(mem_view_addr + r * bpr);
        char line[80];
        int n = snprintf(line, sizeof(line), "$%04X:", a);
        for (int c = 0; c < bpr; c++) {
            n += snprintf(line + n, sizeof(line) - n, " %02X",
                          memory_read(&emu->memory, (uint16_t)(a + c)));
        }
        n += snprintf(line + n, sizeof(line) - n, "  ");
        for (int c = 0; c < bpr; c++) {
            uint8_t b = memory_read(&emu->memory, (uint16_t)(a + c));
            line[n++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        line[n] = '\0';
        mvwprintw(w_mem, r + 1, 2, "%s", line);
    }
    wnoutrefresh(w_mem);
}

static void draw_bps(emulator_t* emu) {
    werase(w_bps);
    draw_box(w_bps, "Breakpoints / Watchpoints");
    int row = 1;
    int max_h, max_w;
    getmaxyx(w_bps, max_h, max_w);

    debugger_t* dbg = &emu->debugger;
    if (dbg->num_breakpoints == 0) {
        mvwprintw(w_bps, row++, 2, "B: (none)");
    } else {
        for (int i = 0; i < dbg->num_breakpoints && row < max_h - 1; i++) {
            breakpoint_t* bp = &dbg->breakpoints[i];
            const char* s = symbol_lookup(&emu->symbols, bp->addr);
            char buf[80];
            if (bp->has_cond) {
                snprintf(buf, sizeof(buf), "B#%d $%04X%s%s if %s",
                         i, bp->addr, s ? " " : "", s ? s : "",
                         bp->cond_text);
            } else {
                snprintf(buf, sizeof(buf), "B#%d $%04X%s%s",
                         i, bp->addr, s ? " " : "", s ? s : "");
            }
            buf[max_w - 4] = '\0';
            wattron(w_bps, COLOR_PAIR(4));
            mvwprintw(w_bps, row++, 2, "%s", buf);
            wattroff(w_bps, COLOR_PAIR(4));
        }
    }

    if (dbg->num_watchpoints == 0) {
        if (row < max_h - 1)
            mvwprintw(w_bps, row++, 2, "W: (none)");
    } else {
        for (int i = 0; i < dbg->num_watchpoints && row < max_h - 1; i++) {
            uint16_t a = dbg->watchpoints[i].addr;
            const char* mode = "wrac";  /* write/read/access/change initial */
            char mc = mode[dbg->watchpoints[i].mode];
            const char* s = symbol_lookup(&emu->symbols, a);
            char buf[80];
            snprintf(buf, sizeof(buf), "W#%d $%04X:%c%s%s",
                     i, a, mc, s ? " " : "", s ? s : "");
            buf[max_w - 4] = '\0';
            mvwprintw(w_bps, row++, 2, "%s", buf);
        }
    }
    wnoutrefresh(w_bps);
}

static void draw_status(const char* msg) {
    werase(w_status);
    wattron(w_status, A_REVERSE);
    mvwprintw(w_status, 0, 0, " %-78s", msg ? msg :
              "s step | n next | c continue | g goto-mem | : cmd | q quit");
    wattroff(w_status, A_REVERSE);
    wnoutrefresh(w_status);
}

static void redraw_all(emulator_t* emu, const char* status) {
    draw_regs(emu);
    draw_stack(emu);
    draw_disasm(emu);
    draw_mem(emu);
    draw_bps(emu);
    draw_status(status);
    doupdate();
}

/* ───── Command-line input via ':' ──────────────────────────────── */

static void prompt_command(emulator_t* emu, char* out, size_t outlen,
                           const char* prefix) {
    werase(w_status);
    mvwprintw(w_status, 0, 0, "%s", prefix);
    wnoutrefresh(w_status);
    doupdate();
    echo();
    curs_set(1);
    wgetnstr(w_status, out, (int)(outlen - 1));
    noecho();
    curs_set(0);
    (void)emu;
}

/* ───── REPL ───────────────────────────────────────────────────── */

void tui_repl(emulator_t* emu) {
    layout_windows();
    debugger_t* dbg = &emu->debugger;

    /* Reset disasm pagination & history (we use PC-anchored disasm here). */
    dbg->disasm_cursor_valid = false;
    dbg->disasm_history_top = 0;
    dbg->active = true;
    dbg->step_mode = false;

    char status[160];
    snprintf(status, sizeof(status), "BREAK at $%04X — ready", emu->cpu.PC);
    redraw_all(emu, status);

    while (dbg->active && emu->running) {
        int ch = wgetch(w_status);
        switch (ch) {
            case 's': case 'S':
                dbg->step_mode = true;
                dbg->active = false;
                return;
            case 'n': case 'N':
                /* Step-over: if current insn is JSR, set temp breakpoint */
                {
                    uint8_t opc = memory_read(&emu->memory, emu->cpu.PC);
                    if (opc == 0x20) {
                        dbg->temp_breakpoint = (uint16_t)(emu->cpu.PC + 3);
                        dbg->has_temp_breakpoint = true;
                        dbg->step_mode = false;
                    } else {
                        dbg->step_mode = true;
                    }
                    dbg->active = false;
                    return;
                }
            case 'c': case 'C':
                dbg->step_mode = false;
                dbg->active = false;
                return;
            case 'q': case 'Q':
                emu->running = false;
                dbg->active = false;
                return;
            case KEY_NPAGE:
                mem_view_addr = (uint16_t)(mem_view_addr + mem_view_rows * 8);
                redraw_all(emu, NULL);
                break;
            case KEY_PPAGE:
                mem_view_addr = (uint16_t)(mem_view_addr - mem_view_rows * 8);
                redraw_all(emu, NULL);
                break;
            case 'g': case 'G': {
                char input[64] = {0};
                prompt_command(emu, input, sizeof(input), "Goto mem $: ");
                if (input[0]) {
                    /* Trim leading $ if present, accept symbol too */
                    char* s = input;
                    while (*s == ' ') s++;
                    uint16_t addr;
                    if (symbol_resolve(&emu->symbols, s, &addr)) {
                        mem_view_addr = addr;
                    } else {
                        if (*s == '$') s++;
                        mem_view_addr = (uint16_t)strtoul(s, NULL, 16);
                    }
                }
                redraw_all(emu, NULL);
                break;
            }
            case ':': {
                char input[256] = {0};
                prompt_command(emu, input, sizeof(input), ": ");
                if (input[0]) {
                    /* Suspend TUI temporarily, run REPL line, restore */
                    def_prog_mode();
                    endwin();
                    debugger_repl_run_line(dbg, emu, input);
                    fflush(stdout);
                    printf("\n[press any key to return to TUI]");
                    fflush(stdout);
                    getchar();
                    reset_prog_mode();
                    refresh();
                    layout_windows();
                }
                redraw_all(emu, NULL);
                break;
            }
            case KEY_RESIZE:
                layout_windows();
                redraw_all(emu, NULL);
                break;
            default:
                break;
        }
    }
}

#endif /* HAS_TUI */
