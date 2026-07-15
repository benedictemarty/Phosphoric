/**
 * @file profiler.h
 * @brief CPU performance profiler — execution hotspots and opcode statistics
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.0.0-alpha
 *
 * Tracks per-address execution counts and cycle usage, opcode frequency,
 * and generates a summary report for performance analysis.
 */

#ifndef PROFILER_H
#define PROFILER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu/cpu6502.h"

/** Maximum number of unique addresses tracked (64K address space) */
#define PROFILER_ADDR_SPACE  65536

/** Number of possible opcodes */
#define PROFILER_OPCODE_COUNT 256

/**
 * @brief CPU performance profiler state
 */
typedef struct {
    bool     active;                            /**< Profiler is active */
    uint64_t total_instructions;                /**< Total instructions profiled */
    uint64_t total_cycles;                      /**< Total cycles profiled */

    /** Per-address execution counts (indexed by 16-bit address) */
    uint32_t addr_hits[PROFILER_ADDR_SPACE];

    /** Per-address cycle counts (indexed by 16-bit address) */
    uint32_t addr_cycles[PROFILER_ADDR_SPACE];

    /** Per-opcode execution counts (indexed by opcode byte) */
    uint32_t opcode_hits[PROFILER_OPCODE_COUNT];
} cpu_profiler_t;

/**
 * @brief Initialize profiler (inactive, all counters zeroed)
 * @param prof Pointer to profiler structure
 */
void profiler_init(cpu_profiler_t* prof);

/**
 * @brief Activate the profiler (start collecting data)
 * @param prof Pointer to profiler structure
 */
void profiler_start(cpu_profiler_t* prof);

/**
 * @brief Deactivate the profiler (stop collecting data)
 * @param prof Pointer to profiler structure
 */
void profiler_stop(cpu_profiler_t* prof);

/**
 * @brief Record one instruction execution (call BEFORE cpu_step)
 *
 * Records the PC address hit and opcode. Call profiler_record_cycles()
 * after cpu_step to record the cycle cost.
 *
 * @param prof Pointer to profiler structure
 * @param cpu Pointer to CPU (reads PC and opcode from memory)
 */
void profiler_record_instruction(cpu_profiler_t* prof, const cpu6502_t* cpu);

/**
 * @brief Record cycle cost of the last instruction
 *
 * @param prof Pointer to profiler structure
 * @param pc Address of the instruction
 * @param cycles Cycles consumed
 */
void profiler_record_cycles(cpu_profiler_t* prof, uint16_t pc, int cycles);

/**
 * @brief Reset all profiler counters to zero
 * @param prof Pointer to profiler structure
 */
void profiler_reset(cpu_profiler_t* prof);

/**
 * @brief Write profiler report to file
 *
 * Outputs:
 * - Summary (total instructions, total cycles)
 * - Execution by memory region (ORIC map buckets + RAM/ROM totals)
 * - Top 20 address hotspots by execution count
 * - Top 20 address hotspots by cycle usage
 * - Opcode frequency histogram (non-zero opcodes)
 *
 * @param prof Pointer to profiler structure
 * @param fp Output file
 */
void profiler_report(const cpu_profiler_t* prof, FILE* fp);

/**
 * @brief Write profiler report to named file
 *
 * @param prof Pointer to profiler structure
 * @param filename Output file path
 * @return true on success
 */
bool profiler_report_to_file(const cpu_profiler_t* prof, const char* filename);

#endif /* PROFILER_H */
