/**
 * @file cpu6502.h
 * @brief MOS 6502 CPU emulation (cycle-accurate)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 *
 * This module implements a cycle-accurate emulation of the MOS 6502 CPU
 * as used in the ORIC-1 computer (running at 1 MHz).
 */

#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration for memory subsystem (avoids circular include) */
typedef struct memory_s memory_t;

/**
 * @brief CPU status flags (Processor Status Register)
 */
typedef enum {
    FLAG_CARRY     = 0x01,  /**< Carry flag (C) */
    FLAG_ZERO      = 0x02,  /**< Zero flag (Z) */
    FLAG_INTERRUPT = 0x04,  /**< Interrupt disable (I) */
    FLAG_DECIMAL   = 0x08,  /**< Decimal mode (D) */
    FLAG_BREAK     = 0x10,  /**< Break command (B) */
    FLAG_UNUSED    = 0x20,  /**< Unused (always 1) */
    FLAG_OVERFLOW  = 0x40,  /**< Overflow flag (V) */
    FLAG_NEGATIVE  = 0x80   /**< Negative flag (N) */
} cpu_flags_t;

/**
 * @brief IRQ source flags (bitfield for level-triggered IRQ model)
 *
 * The real 6502 IRQ line is level-triggered: the CPU takes an interrupt
 * whenever IRQ is asserted AND the I flag is clear. Multiple sources
 * can assert IRQ simultaneously. Each source sets/clears its own bit.
 */
typedef enum {
    IRQF_VIA     = 0x01,  /**< VIA 6522 IRQ (T1 timer, CB1/CB2, etc.) */
    IRQF_DISK    = 0x02,  /**< Microdisc FDC INTRQ */
    IRQF_SERIAL  = 0x04,  /**< ACIA 6551 serial IRQ */
    IRQF_DTL2000 = 0x08,  /**< Digitelec DTL 2000 ACIA 6850 IRQ */
    IRQF_MAGECO  = 0x10,  /**< Mageco MIDI interface ACIA 6850 IRQ */
    IRQF_ULANG   = 0x20   /**< ULA-NG raster IRQ (NG_RASTERLINE / NG_STATUS) */
} cpu_irq_source_t;

/**
 * @brief CPU state structure
 */
typedef struct {
    uint8_t  A;             /**< Accumulator */
    uint8_t  X;             /**< X register */
    uint8_t  Y;             /**< Y register */
    uint8_t  SP;            /**< Stack Pointer (0x0100 + SP) */
    uint16_t PC;            /**< Program Counter */
    uint8_t  P;             /**< Processor Status Register */

    uint64_t cycles;        /**< Total cycles executed */
    uint32_t cycles_left;   /**< Cycles remaining for current instruction */

    bool     halted;        /**< CPU halted flag */
    bool     nmi_pending;   /**< Non-Maskable Interrupt pending */
    uint8_t  irq;           /**< IRQ source bitfield (level-triggered) */

    memory_t* memory;       /**< Pointer to memory subsystem */

    /* Optional IRQ trace: when non-NULL, handle_irq and RTI opcode log
     * one line per event. Caller owns the FILE* lifecycle. */
    void* irq_trace_fp;     /**< FILE* (opaque to avoid <stdio.h> coupling) */
    uint64_t irq_trace_count;

    /* Optional per-cycle clock callback. When set, the CPU calls it for every
     * bus cycle it consumes (one call per memory access, plus a final call for
     * an instruction's internal cycles), letting peripherals advance in step
     * with the CPU's bus accesses instead of in one post-instruction batch.
     * When NULL the CPU only accumulates cycles (callers tick peripherals). */
    void (*on_cycle)(void* ctx, int cycles);
    void* cycle_ctx;
} cpu6502_t;

/**
 * @brief Addressing modes
 */
typedef enum {
    ADDR_IMPLICIT,      /**< Implicit (no operand) */
    ADDR_ACCUMULATOR,   /**< Accumulator */
    ADDR_IMMEDIATE,     /**< Immediate (#$nn) */
    ADDR_ZERO_PAGE,     /**< Zero Page ($nn) */
    ADDR_ZERO_PAGE_X,   /**< Zero Page,X ($nn,X) */
    ADDR_ZERO_PAGE_Y,   /**< Zero Page,Y ($nn,Y) */
    ADDR_RELATIVE,      /**< Relative (branch) */
    ADDR_ABSOLUTE,      /**< Absolute ($nnnn) */
    ADDR_ABSOLUTE_X,    /**< Absolute,X ($nnnn,X) */
    ADDR_ABSOLUTE_Y,    /**< Absolute,Y ($nnnn,Y) */
    ADDR_INDIRECT,      /**< Indirect (JMP) */
    ADDR_INDEXED_INDIRECT,  /**< Indexed Indirect ($nn,X) */
    ADDR_INDIRECT_INDEXED   /**< Indirect Indexed ($nn),Y */
} addressing_mode_t;

/**
 * @brief Initialize CPU state
 *
 * @param cpu Pointer to CPU structure
 * @param memory Pointer to memory subsystem
 */
void cpu_init(cpu6502_t* cpu, memory_t* memory);

/**
 * @brief Reset CPU (power-on or reset button)
 *
 * @param cpu Pointer to CPU structure
 */
void cpu_reset(cpu6502_t* cpu);

/**
 * @brief Execute one instruction
 *
 * @param cpu Pointer to CPU structure
 * @return Number of cycles consumed
 */
int cpu_step(cpu6502_t* cpu);

/**
 * @brief Execute N cycles
 *
 * @param cpu Pointer to CPU structure
 * @param cycles Number of cycles to execute
 * @return Actual cycles executed
 */
int cpu_execute_cycles(cpu6502_t* cpu, int cycles);

/**
 * @brief Set the per-cycle clock callback (or NULL to disable).
 *
 * @param cpu Pointer to CPU structure
 * @param cb Callback invoked with the number of cycles for each bus tick
 * @param ctx User context passed to the callback
 */
void cpu_set_cycle_callback(cpu6502_t* cpu, void (*cb)(void*, int), void* ctx);

/**
 * @brief Trigger NMI (Non-Maskable Interrupt)
 *
 * @param cpu Pointer to CPU structure
 */
void cpu_nmi(cpu6502_t* cpu);

/**
 * @brief Assert IRQ source (level-triggered)
 *
 * Sets the specified IRQ source bit. The CPU will take an interrupt
 * whenever any IRQ bit is set and the I flag is clear.
 *
 * @param cpu Pointer to CPU structure
 * @param source IRQ source flag (IRQF_VIA, IRQF_DISK, etc.)
 */
void cpu_irq_set(cpu6502_t* cpu, cpu_irq_source_t source);

/**
 * @brief Deassert IRQ source (level-triggered)
 *
 * Clears the specified IRQ source bit. If no other IRQ sources
 * remain asserted, the CPU will not take further interrupts.
 *
 * @param cpu Pointer to CPU structure
 * @param source IRQ source flag to clear
 */
void cpu_irq_clear(cpu6502_t* cpu, cpu_irq_source_t source);

/**
 * @brief Trigger IRQ (legacy edge-triggered, deprecated)
 *
 * @param cpu Pointer to CPU structure
 * @deprecated Use cpu_irq_set() / cpu_irq_clear() instead
 */
void cpu_irq(cpu6502_t* cpu);

/**
 * @brief Set/clear CPU flag
 *
 * @param cpu Pointer to CPU structure
 * @param flag Flag to modify
 * @param value true to set, false to clear
 */
void cpu_set_flag(cpu6502_t* cpu, cpu_flags_t flag, bool value);

/**
 * @brief Get CPU flag state
 *
 * @param cpu Pointer to CPU structure
 * @param flag Flag to check
 * @return true if set, false if clear
 */
bool cpu_get_flag(const cpu6502_t* cpu, cpu_flags_t flag);

/**
 * @brief Disassemble instruction at address
 *
 * @param cpu Pointer to CPU structure
 * @param address Address to disassemble
 * @param buffer Output buffer for disassembly string
 * @param buffer_size Size of output buffer
 * @return Number of bytes consumed by instruction
 */
int cpu_disassemble(const cpu6502_t* cpu, uint16_t address, char* buffer, size_t buffer_size);

/**
 * @brief Get base cycle count for an opcode.
 * Does not include branch-taken/page-cross penalties.
 */
uint8_t cpu_opcode_cycles(uint8_t opcode);

/**
 * @brief Get CPU state as string (for debugging)
 *
 * @param cpu Pointer to CPU structure
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 */
void cpu_get_state_string(const cpu6502_t* cpu, char* buffer, size_t buffer_size);

#endif /* CPU6502_H */
