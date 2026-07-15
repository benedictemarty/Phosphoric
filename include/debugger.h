/**
 * @file debugger.h
 * @brief Interactive debugger for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.1.0-alpha
 *
 * Provides breakpoints, watchpoints, single-step, and REPL
 * command interface for debugging ORIC programs.
 */

#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <stdint.h>
#include <stdbool.h>

#define DEBUGGER_MAX_BREAKPOINTS 16
#define DEBUGGER_MAX_WATCHPOINTS 8

/* Forward declaration for emulator (avoids circular include) */
typedef struct emulator_s emulator_t;

/* Conditional-breakpoint operand and comparison. */
typedef enum {
    BP_OPERAND_NONE = 0,
    BP_OPERAND_A,    BP_OPERAND_X,    BP_OPERAND_Y,
    BP_OPERAND_SP,   BP_OPERAND_P,    BP_OPERAND_PC,
    BP_OPERAND_MEM   /* M[addr] */
} bp_operand_t;

typedef enum {
    BP_OP_EQ, BP_OP_NE, BP_OP_LT, BP_OP_LE, BP_OP_GT, BP_OP_GE
} bp_op_t;

typedef struct {
    bp_operand_t operand;
    uint16_t     mem_addr;   /* Only used when operand == BP_OPERAND_MEM */
    bp_op_t      op;
    uint32_t     value;
} bp_condition_t;

/* Compound condition: up to BP_MAX_TERMS single comparisons joined by AND/OR
 * connectors, evaluated strictly left-to-right (no operator precedence). */
#define BP_MAX_TERMS 4
typedef enum { BP_CONN_AND = 0, BP_CONN_OR } bp_conn_t;

typedef struct {
    bp_condition_t terms[BP_MAX_TERMS];
    bp_conn_t      conn[BP_MAX_TERMS - 1];  /* conn[i] joins terms[i] and terms[i+1] */
    uint8_t        num_terms;
} bp_condexpr_t;

typedef struct {
    uint16_t       addr;
    bool           has_cond;
    bp_condexpr_t  cond;
    char           cond_text[96];   /* Verbatim user text for listing */
} breakpoint_t;

/* Watchpoint access mode. Mirrors gdb Z2/Z3/Z4 and Oricutron bsm r/w/c. */
typedef enum {
    WATCH_WRITE = 0,   /* break on write */
    WATCH_READ,        /* break on read */
    WATCH_ACCESS,      /* break on read or write */
    WATCH_CHANGE       /* break only when a write changes the stored value */
} watch_mode_t;

typedef struct {
    uint16_t     addr;
    watch_mode_t mode;
    uint8_t      last_value;   /* WATCH_CHANGE: last observed value */
    bool         has_last;
} watchpoint_t;

typedef struct debugger_s {
    breakpoint_t breakpoints[DEBUGGER_MAX_BREAKPOINTS];
    uint8_t      num_breakpoints;

    watchpoint_t watchpoints[DEBUGGER_MAX_WATCHPOINTS];
    uint8_t      num_watchpoints;

    /* Sprint 34d4 (P2-G audit) — raster-line breakpoints. Each entry holds
     * a PAL line number 0..311; the debugger fires when frame_cycles crosses
     * its threshold. last_raster_line tracks the previous observation so a
     * single CPU step (≤ 7 cycles) that straddles a line boundary still
     * triggers. -1 = no observation yet. */
    int16_t  raster_bps[8];
    uint8_t  num_raster_bps;
    int16_t  last_raster_line;

    /* Sprint 34d5 (P2-F audit) — single-step rewind ring. Snapshots
     * (CPU + 64KB RAM + 16KB upper_ram + memory flags + frame position)
     * are pushed before each `s`/`n` step. `u` pops and restores. The
     * actual storage lives in a file-static array in debugger.c (~1.3 MB
     * for 16 slots) so debugger_t itself stays small. Peripheral state
     * (VIA / PSG / FDC / LOCI / ACIA) is NOT rewound — document limitation. */
    uint8_t  undo_head;     /* next slot to write (0..15) */
    uint8_t  undo_count;    /* entries actually present (0..16) */

    bool     watch_triggered;    /* Set by memory trace callback */
    uint16_t watch_addr_hit;     /* Which watchpoint address was hit */
    bool     watch_read_hit;     /* True if the hit was a read access */

    bool     active;             /* Debugger is in REPL mode */
    bool     step_mode;          /* Single-step after break */

    /* Sprint 35b — explicit last-break reason for the IPC control mode.
     * Set by debugger_should_break (which consumes the transient flags
     * like watch_triggered) and read by the main loop right after, so
     * `EVT stopped reason=…` carries the correct cause.
     * Values: "step", "watch", "raster", "break" (PC bp), "temp" (next).
     * Empty string when none. */
    char     last_break_reason[16];

    /* Temporary breakpoint for step-over (next command) */
    uint16_t temp_breakpoint;
    bool     has_temp_breakpoint;

    /* Disassembler pagination state */
    uint16_t disasm_cursor;          /* Next page starts here */
    uint8_t  disasm_count;           /* Page size (default 10) */
    bool     disasm_cursor_valid;    /* False until first `d` */
    uint16_t disasm_history[16];     /* Ring buffer of prior cursors */
    uint8_t  disasm_history_top;     /* Number of entries currently held */
} debugger_t;

/**
 * @brief Initialize debugger state
 */
void debugger_init(debugger_t* dbg);

/**
 * @brief Check if debugger should break before next instruction
 *
 * Called before each cpu_step(). Returns true if the debugger
 * should enter REPL mode (breakpoint hit, watchpoint triggered,
 * step mode, etc.)
 */
bool debugger_should_break(debugger_t* dbg, emulator_t* emu);

/**
 * @brief Interactive REPL command loop
 *
 * Reads commands from stdin and executes them.
 * Returns when the user continues execution (c/continue).
 */
void debugger_repl(debugger_t* dbg, emulator_t* emu);

/**
 * @brief Execute a single REPL command line programmatically.
 *
 * Same dispatch as the interactive REPL (and the TUI ':' line), driven from
 * a string instead of stdin. Used by the TUI command line, scripting and
 * unit tests. No-op on NULL/empty input.
 */
void debugger_repl_run_line(debugger_t* dbg, emulator_t* emu, const char* line);

/**
 * @brief Add a PC breakpoint
 * @return Index of added breakpoint, or -1 if full
 */
int debugger_add_breakpoint(debugger_t* dbg, uint16_t addr);

/**
 * @brief Remove a PC breakpoint by index
 * @return true if removed, false if index out of range
 */
bool debugger_remove_breakpoint(debugger_t* dbg, int index);

/**
 * @brief Add a PC breakpoint carrying a compound condition ("A==5 && X==3").
 * @return Breakpoint index, -1 if the table is full, -2 if the expression is
 *         unparseable.
 */
int debugger_add_cond_breakpoint(debugger_t* dbg, const emulator_t* emu,
                                 uint16_t addr, const char* expr);

/* ── Iterative memory search (cheat-finder) — shared by REPL and --control ── */
typedef enum { HUNT_EQ, HUNT_UNCHANGED, HUNT_CHANGED, HUNT_GT, HUNT_LT } hunt_pred_t;

void     debugger_hunt_start(emulator_t* emu);                        /* seed: all cells */
uint32_t debugger_hunt_refine(emulator_t* emu, hunt_pred_t pred, uint8_t val);
uint32_t debugger_hunt_count(void);
bool     debugger_hunt_active(void);
void     debugger_hunt_clear(void);
/* Fill up to @max candidate addresses (and values if @out_vals != NULL);
 * returns how many were written. */
uint32_t debugger_hunt_list(emulator_t* emu, uint16_t* out, uint32_t max, uint8_t* out_vals);

/* ── Memory ⇄ file region helpers ── */
bool debugger_save_region(emulator_t* emu, const char* path, uint16_t addr, uint32_t len);
long debugger_load_region(emulator_t* emu, const char* path, uint16_t addr);

/**
 * @brief Add a memory write watchpoint (WATCH_WRITE mode)
 * @return Index of added watchpoint, or -1 if full
 */
int debugger_add_watchpoint(debugger_t* dbg, uint16_t addr);

/**
 * @brief Add a memory watchpoint with an explicit access mode
 * @return Index of added watchpoint, or -1 if full
 */
int debugger_add_watchpoint_mode(debugger_t* dbg, uint16_t addr, watch_mode_t mode);

/**
 * @brief Remove a watchpoint by index
 * @return true if removed, false if index out of range
 */
bool debugger_remove_watchpoint(debugger_t* dbg, int index);

/**
 * @brief Check if a given PC address matches any breakpoint
 */
bool debugger_is_breakpoint(const debugger_t* dbg, uint16_t pc);

/**
 * @brief Install memory trace callback for watchpoints
 */
void debugger_install_watchpoint_trace(debugger_t* dbg, emulator_t* emu);

#endif /* DEBUGGER_H */
