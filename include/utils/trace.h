/**
 * @file trace.h
 * @brief CPU trace logging — instruction-level execution trace to file
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.0.0-alpha
 *
 * Logs each CPU instruction with disassembly and register state.
 * Output format (one line per instruction):
 *   CCCCCCCC  AAAA  XX XX XX  MNEMONIC OPERAND       A=XX X=XX Y=XX SP=XX P=XX
 */

#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu/cpu6502.h"
#include "utils/symbols.h"   /* symbol_table_t for optional inline symbols */

struct memory_s;         /* forward decl for the trace memory hook */

#define TRACE_LINE_MAX 128   /**< Fixed width of one formatted ring entry */

/* Conditional-trace start/stop triggers (Epic 6 / US 1). */
typedef enum { TRACE_START_NOW = 0, TRACE_START_PC } trace_start_t;
typedef enum {
    TRACE_STOP_NONE = 0,
    TRACE_STOP_CYCLE,   /**< stop N cycles after recording began */
    TRACE_STOP_BRK,     /**< stop on the next BRK ($00) */
    TRACE_STOP_WRITE,   /**< stop on a write to stop_addr */
    TRACE_STOP_READ     /**< stop on a read from stop_addr */
} trace_stop_t;

/**
 * @brief CPU trace logger state
 */
typedef struct {
    FILE*    fp;            /**< Output file (NULL = inactive) */
    bool     active;        /**< Trace is recording */
    uint64_t count;         /**< Number of instructions traced */
    uint64_t max_count;     /**< Max instructions to trace (0 = unlimited) */
    bool     owns_fp;       /**< True if we opened the file (must fclose) */

    /* ── Conditional tracing (Epic 6 / US 1) ── */
    bool          armed;        /**< Configured; waiting for the start trigger */
    trace_start_t start_cond;
    uint16_t      start_pc;
    trace_stop_t  stop_cond;
    uint16_t      stop_addr;
    uint64_t      stop_cycle;   /**< delta (cycles) for TRACE_STOP_CYCLE */
    uint64_t      start_cycle;  /**< captured on the first recorded instruction */
    bool          stop_hit;     /**< set when a stop trigger fired */

    /* Circular buffer (keeps only the last ring_cap instructions). */
    char*    ring;
    uint32_t ring_cap;
    uint32_t ring_head;
    uint32_t ring_count;

    bool     with_symbols;
    const symbol_table_t* symbols;
} cpu_trace_t;

/**
 * @brief Initialize trace logger (inactive by default)
 * @param trace Pointer to trace structure
 */
void trace_init(cpu_trace_t* trace);

/**
 * @brief Open trace output file and activate tracing
 * @param trace Pointer to trace structure
 * @param filename Output file path (NULL for stdout)
 * @return true on success
 */
bool trace_open(cpu_trace_t* trace, const char* filename);

/**
 * @brief Attach an already-opened FILE* for tracing
 * @param trace Pointer to trace structure
 * @param fp File pointer (caller retains ownership)
 */
void trace_attach(cpu_trace_t* trace, FILE* fp);

/**
 * @brief Log one instruction (call BEFORE cpu_step)
 *
 * Captures current PC, disassembles the instruction, and logs
 * the full register state in a single line.
 *
 * @param trace Pointer to trace structure
 * @param cpu Pointer to CPU (const — does not modify state)
 */
void trace_log_instruction(cpu_trace_t* trace, const cpu6502_t* cpu);

/**
 * @brief Close trace file and deactivate tracing
 * @param trace Pointer to trace structure
 */
void trace_close(cpu_trace_t* trace);

/**
 * @brief Set maximum number of instructions to trace
 * @param trace Pointer to trace structure
 * @param max Maximum count (0 = unlimited)
 */
void trace_set_max(cpu_trace_t* trace, uint64_t max);

/* ── Conditional tracing (Epic 6 / US 1) ────────────────────────────── */

/**
 * @brief Arm a conditional trace (start/stop triggers + optional ring buffer).
 *
 * With TRACE_START_NOW recording begins immediately; with TRACE_START_PC it
 * begins when the CPU first reaches @p start_pc. If @p ring_cap > 0 the trace
 * keeps only the last @p ring_cap instructions in memory (save with
 * trace_save_ring); otherwise it streams to the attached FILE*.
 */
void trace_arm(cpu_trace_t* trace, trace_start_t start_cond, uint16_t start_pc,
               trace_stop_t stop_cond, uint16_t stop_addr, uint64_t stop_cycle,
               uint32_t ring_cap, bool with_symbols);

/** @brief Provide a symbol table for inline symbol annotation. */
void trace_set_symbols(cpu_trace_t* trace, const symbol_table_t* syms);

/** @brief Notify the trace of a memory access (drives write/read stop triggers). */
void trace_note_mem_access(cpu_trace_t* trace, uint16_t addr, int is_write);

/** @brief Dump the ring buffer (oldest → newest) to @p filename. */
bool trace_save_ring(cpu_trace_t* trace, const char* filename);

/** @brief Number of instructions currently held in the ring buffer. */
uint32_t trace_ring_count(const cpu_trace_t* trace);

/** @brief Disarm/stop and release the ring buffer (keeps any open FILE*). */
void trace_reset(cpu_trace_t* trace);

/** @brief Stop recording but keep the ring buffer (so it can still be saved). */
void trace_stop(cpu_trace_t* trace);

/** @brief Install/refresh the memory hook used by write/read stop triggers. */
void trace_install_mem_hook(cpu_trace_t* trace, struct memory_s* mem);

/** @brief Parse a trace spec string (see trace.c) into trace_arm() arguments. */
bool trace_parse_spec(const char* args,
                      trace_start_t* start, uint16_t* start_pc,
                      trace_stop_t* stop, uint16_t* stop_addr, uint64_t* stop_cycle,
                      uint32_t* ring_cap, bool* with_sym);

#endif /* TRACE_H */
