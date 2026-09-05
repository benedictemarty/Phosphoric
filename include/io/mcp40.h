/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file mcp40.h
 * @brief MCP-40 4-color pen plotter emulation for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.8.0-alpha
 *
 * Emulates the MCP-40 (Tandy CGP-115 / Sharp CE-150) 4-color pen plotter.
 * The MCP-40 receives ASCII commands via the Centronics parallel port
 * and draws on a virtual paper using 4 colored pens.
 *
 * Command protocol:
 *   I           Initialize (reset plotter)
 *   H           Home (pen to origin 0,0)
 *   D x,y[,...] Draw to (x,y) with pen down
 *   M x,y[,...] Move to (x,y) with pen up
 *   J c         Select pen color (0=black, 1=blue, 2=green, 3=red)
 *   P char      Print character at current position
 *   L type      Line type (0=solid, 1-4=dashed)
 *   Q size      Character size (1-63)
 *
 * Plotter resolution: 480 x 400 dots.
 * Output: BMP file on close or explicit export.
 */

#ifndef MCP40_H
#define MCP40_H

#include <stdint.h>
#include <stdbool.h>

/** Plotter dimensions */
#define MCP40_WIDTH   480
#define MCP40_HEIGHT  400

/** Pen colors */
typedef enum {
    MCP40_BLACK = 0,
    MCP40_BLUE  = 1,
    MCP40_GREEN = 2,
    MCP40_RED   = 3
} mcp40_color_t;

/** Line types */
typedef enum {
    MCP40_LINE_SOLID  = 0,
    MCP40_LINE_DASH1  = 1,
    MCP40_LINE_DASH2  = 2,
    MCP40_LINE_DASH3  = 3,
    MCP40_LINE_DASH4  = 4
} mcp40_linetype_t;

/** Parser state machine */
typedef enum {
    MCP40_IDLE,       /**< Waiting for command letter */
    MCP40_PARAM       /**< Collecting parameters */
} mcp40_parse_state_t;

/**
 * @brief MCP-40 plotter state
 */
typedef struct mcp40_s {
    /* Framebuffer (RGB, 3 bytes per pixel) */
    uint8_t framebuffer[MCP40_WIDTH * MCP40_HEIGHT * 3];

    /* Pen state */
    int pen_x;                  /**< Current X position (0..479) */
    int pen_y;                  /**< Current Y position (0..399) */
    mcp40_color_t color;        /**< Active pen color */
    mcp40_linetype_t line_type; /**< Line style */
    int char_size;              /**< Character size (1-63) */

    /* Command parser */
    mcp40_parse_state_t state;
    char current_cmd;           /**< Current command being parsed */
    char param_buf[128];        /**< Parameter accumulation buffer */
    int param_len;              /**< Bytes in param_buf */

    /* Status */
    uint32_t line_count;        /**< Lines drawn */
    uint32_t char_count;        /**< Characters printed */
    bool dirty;                 /**< Framebuffer modified since last export */

    /* Output file */
    const char* output_file;    /**< BMP output filename */
} mcp40_t;

/** Initialize MCP-40 plotter (white paper, pen at origin) */
void mcp40_init(mcp40_t* mcp);

/** Reset plotter to initial state (clears paper) */
void mcp40_reset(mcp40_t* mcp);

/**
 * @brief Receive a byte from the Centronics port
 *
 * Feeds one byte into the command parser. Commands are accumulated
 * and executed when complete (next command letter or CR/LF).
 *
 * @param mcp  Plotter state
 * @param byte The byte received from VIA Port A
 */
void mcp40_receive_byte(mcp40_t* mcp, uint8_t byte);

/** Export framebuffer to BMP file. Returns true on success. */
bool mcp40_export_bmp(const mcp40_t* mcp, const char* filename);

/** Set output filename (auto-export on close) */
void mcp40_set_output(mcp40_t* mcp, const char* filename);

/** Get RGB color for a pen */
void mcp40_get_pen_rgb(mcp40_color_t color, uint8_t* r, uint8_t* g, uint8_t* b);

#endif /* MCP40_H */
