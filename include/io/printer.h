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

/** CA2 control mode: forced low (STROBE asserted) */
#define CA2_FORCED_LOW   0x06
/** CA2 control mode: forced high (STROBE released) */
#define CA2_FORCED_HIGH  0x07

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
 * @brief Check for STROBE transition on PCR CA2 write
 *
 * Called when the CPU writes to VIA PCR. Detects the CA2 forced-low →
 * forced-high transition that constitutes a Centronics STROBE pulse.
 * When detected, captures the current VIA ORA value to the output file.
 *
 * @param printer  Printer state
 * @param old_pcr  Previous PCR value (before write)
 * @param new_pcr  New PCR value (being written)
 * @param data     VIA ORA value (the data byte on Port A)
 */
void oric_printer_check_strobe(oric_printer_t* printer, uint8_t old_pcr,
                                uint8_t new_pcr, uint8_t data);

/** Flush printer output */
void oric_printer_flush(oric_printer_t* printer);

/** Returns true if printer is active (file open) */
bool oric_printer_is_active(const oric_printer_t* printer);

#endif /* PRINTER_H */
