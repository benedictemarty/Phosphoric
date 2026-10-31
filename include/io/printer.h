/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file printer.h
 * @brief Centronics parallel printer interface for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.8.0-alpha
 *
 * Emulates the Centronics parallel printer interface used by the ORIC.
 * The ORIC sends data via VIA Port A (ORA) and signals STROBE via CA2.
 *
 * Protocol:
 *   1. CPU writes data byte to VIA ORA ($0301)
 *   2. CPU sets PCR CA2 mode to "forced low" (bits 1-3 = 110) = STROBE asserted
 *   3. CPU sets PCR CA2 mode to "forced high" (bits 1-3 = 111) = STROBE released
 *   4. On rising edge (low→high), the data byte is latched and output
 *
 * Supports two printer types:
 *   - TEXT: raw byte capture to text file (LPRINT/LLIST)
 *   - MCP40: MCP-40 4-color pen plotter with graphical BMP output
 */

#ifndef PRINTER_H
#define PRINTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "io/mcp40.h"

/** Centronics STROBE line: VIA port B bit 4 (PB4), active low */
#define PRINTER_STROBE_PB4  0x10

/** Printer type */
typedef enum {
    PRINTER_NONE = 0,   /**< No printer */
    PRINTER_TEXT,        /**< Text capture (LPRINT/LLIST → text file) */
    PRINTER_MCP40       /**< MCP-40 4-color plotter (→ BMP output) */
} oric_printer_type_t;

/**
 * @brief Printer state
 */
typedef struct oric_printer_s {
    FILE* output;              /**< Output file for TEXT mode (NULL = disabled) */
    const char* filename;      /**< Output filename for display */
    bool strobe_low;           /**< CA2 was forced low (STROBE asserted) */
    uint32_t byte_count;       /**< Number of bytes printed/sent */
    oric_printer_type_t type;  /**< Printer type (text or MCP-40) */
    mcp40_t mcp40;             /**< MCP-40 plotter state (when type == PRINTER_MCP40) */
} oric_printer_t;

/** Initialize printer (disabled state) */
void oric_printer_init(oric_printer_t* printer);

/** Open printer output file. Returns true on success. */
bool oric_printer_open(oric_printer_t* printer, const char* filename);

/** Close printer output file */
void oric_printer_close(oric_printer_t* printer);

/**
 * @brief Centronics STROBE on VIA PB4 (2.31.4).
 *
 * Called on every write to VIA ORB with the PB4 output level before and
 * after it. The Oric ROMs print a byte by writing it to ORA ($0301), then
 * pulling PB4 low and back high (Atmos $F5C1: AND #$EF / STA $0300, ORA #$10
 * / STA $0300; ORIC-1 $F57B via the $F56A helper), then wait for the ACK on
 * CA1 (IFR bit 1). PB4 high after a low level latches @p data.
 *
 * Before 2.31.4 the strobe was taken from PCR CA2, which is the PSG BC1
 * line: the file only got PSG register numbers, and LPRINT hung waiting
 * for an ACK that never came.
 *
 * @param printer  Printer state
 * @param old_pb4  PB4 output level before the ORB write
 * @param new_pb4  PB4 output level after it
 * @param data     VIA ORA value (the data byte on Port A)
 * @return true when a byte was latched: the caller then pulses the ACK
 *         (CA1 low then high) so the ROM print loop can go on.
 */
bool oric_printer_check_pb4(oric_printer_t* printer, bool old_pb4,
                            bool new_pb4, uint8_t data);

/** Flush printer output */
void oric_printer_flush(oric_printer_t* printer);

/** Returns true if printer is active (file open) */
bool oric_printer_is_active(const oric_printer_t* printer);

#endif /* PRINTER_H */
