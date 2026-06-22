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

typedef struct {
    uint16_t       addr;
    bool           has_cond;
    bp_condition_t cond;
    char           cond_text[48];   /* Verbatim user text for listing */
} breakpoint_t;

typedef struct debugger_s {
    breakpoint_t breakpoints[DEBUGGER_MAX_BREAKPOINTS];
    uint8_t      num_breakpoints;

    uint16_t watchpoints[DEBUGGER_MAX_WATCHPOINTS];
    uint8_t  num_watchpoints;

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
 * @brief Add a memory write watchpoint
 * @return Index of added watchpoint, or -1 if full
 */
int debugger_add_watchpoint(debugger_t* dbg, uint16_t addr);

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
