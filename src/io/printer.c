/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file printer.c
 * @brief Centronics parallel printer interface for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.8.0-alpha
 *
 * Captures ORIC printer output via Centronics STROBE detection.
 * Supports two modes:
 *   - TEXT: raw byte capture to text file (LPRINT/LLIST)
 *   - MCP40: 4-color pen plotter with graphical BMP output
 */

#include "io/printer.h"
#include "utils/logging.h"

#include <string.h>

void oric_printer_init(oric_printer_t* printer)
{
    memset(printer, 0, sizeof(oric_printer_t) -
           sizeof(mcp40_t));  /* Don't clear large MCP-40 framebuffer yet */
    printer->output = NULL;
    printer->filename = NULL;
    printer->strobe_low = false;
    printer->byte_count = 0;
    printer->type = PRINTER_NONE;
    mcp40_init(&printer->mcp40);
}

bool oric_printer_open(oric_printer_t* printer, const char* filename)
{
    if (!printer || !filename) return false;

    /* Close previous file if any */
    if (printer->output || printer->type == PRINTER_MCP40) {
        oric_printer_close(printer);
    }

    /* Default to TEXT mode if no type specified */
    if (printer->type == PRINTER_NONE) {
        printer->type = PRINTER_TEXT;
    }

    if (printer->type == PRINTER_MCP40) {
        /* MCP-40 mode: no text file, BMP exported on close */
        mcp40_init(&printer->mcp40);
        mcp40_set_output(&printer->mcp40, filename);
        printer->filename = filename;
        printer->byte_count = 0;
        printer->strobe_low = false;
        log_info("printer: MCP-40 plotter output to '%s'", filename);
        return true;
    }

    /* TEXT mode: open text file */
    printer->output = fopen(filename, "w");
    if (!printer->output) {
        log_error("printer: cannot open '%s' for writing", filename);
        return false;
    }

    printer->filename = filename;
    printer->byte_count = 0;
    printer->strobe_low = false;
    log_info("printer: text output to '%s'", filename);
    return true;
}

void oric_printer_close(oric_printer_t* printer)
{
    if (!printer) return;

    if (printer->type == PRINTER_MCP40) {
        /* Export BMP on close if plotter has drawn anything */
        if (printer->mcp40.dirty && printer->mcp40.output_file) {
            mcp40_export_bmp(&printer->mcp40, printer->mcp40.output_file);
        }
        log_info("printer: MCP-40 closed (%u bytes, %u lines, %u chars)",
                 printer->byte_count, printer->mcp40.line_count,
                 printer->mcp40.char_count);
    } else if (printer->output) {
        fflush(printer->output);
        fclose(printer->output);
        log_info("printer: closed '%s' (%u bytes printed)",
                 printer->filename ? printer->filename : "?",
                 printer->byte_count);
        printer->output = NULL;
    }
    printer->filename = NULL;
    printer->strobe_low = false;
}

void oric_printer_check_strobe(oric_printer_t* printer, uint8_t old_pcr,
                                uint8_t new_pcr, uint8_t data)
{
    if (!printer) return;

    /* Must have an active printer (text file or MCP-40) */
    if (printer->type == PRINTER_NONE) return;
    if (printer->type == PRINTER_TEXT && !printer->output) return;

    /* Extract CA2 control mode (PCR bits 1-3) */
    uint8_t old_ca2 = (old_pcr >> 1) & 0x07;
    uint8_t new_ca2 = (new_pcr >> 1) & 0x07;

    /*
     * Detect STROBE sequence:
     *   Step 1: CA2 forced low (mode 110 = 0x06) — STROBE asserted
     *   Step 2: CA2 forced high (mode 111 = 0x07) — STROBE released
     *
     * The rising edge (low → high) latches the data byte.
     */
    if (new_ca2 == CA2_FORCED_LOW) {
        printer->strobe_low = true;
    } else if (new_ca2 == CA2_FORCED_HIGH && printer->strobe_low) {
        /* Rising edge: capture the byte */
        printer->byte_count++;
        printer->strobe_low = false;

        if (printer->type == PRINTER_MCP40) {
            /* Route byte to MCP-40 command parser */
            mcp40_receive_byte(&printer->mcp40, data);
        } else {
            /* TEXT mode: write byte to file */
            fputc(data, printer->output);
            /* Flush periodically (every newline for text readability) */
            if (data == '\n' || data == '\r') {
                fflush(printer->output);
            }
        }
    } else if (old_ca2 != new_ca2) {
        /* CA2 mode changed to something else — reset strobe tracking */
        printer->strobe_low = false;
    }
}

void oric_printer_flush(oric_printer_t* printer)
{
    if (printer && printer->output) {
        fflush(printer->output);
    }
}

bool oric_printer_is_active(const oric_printer_t* printer)
{
    if (!printer) return false;
    if (printer->type == PRINTER_MCP40) return true;
    return printer->output != NULL;
}
