/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file mcp40.c
 * @brief MCP-40 4-color pen plotter emulation for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.8.0-alpha
 *
 * Emulates the MCP-40 pen plotter command protocol.
 * Receives bytes one at a time from Centronics STROBE, parses commands,
 * and renders to an internal RGB framebuffer (480x400).
 */

#include "io/mcp40.h"
#include "utils/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════ */
/*  Pen color RGB values                                           */
/* ═══════════════════════════════════════════════════════════════ */

void mcp40_get_pen_rgb(mcp40_color_t color, uint8_t* r, uint8_t* g, uint8_t* b)
{
    switch (color) {
        case MCP40_BLACK: *r = 0;   *g = 0;   *b = 0;   break;
        case MCP40_BLUE:  *r = 0;   *g = 0;   *b = 200; break;
        case MCP40_GREEN: *r = 0;   *g = 160; *b = 0;   break;
        case MCP40_RED:   *r = 200; *g = 0;   *b = 0;   break;
        default:          *r = 0;   *g = 0;   *b = 0;   break;
    }
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Built-in 5x7 bitmap font for P command                        */
/* ═══════════════════════════════════════════════════════════════ */

/* Simple 5x7 font glyphs for printable ASCII (32-126) */
static const uint8_t font_5x7[95][7] = {
    /* ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* '!' */ {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    /* '"' */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* '#' */ {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00},
    /* '$' */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* '%' */ {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    /* '&' */ {0x08,0x14,0x14,0x08,0x15,0x12,0x0D},
    /* ''' */ {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    /* '(' */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    /* ')' */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    /* '*' */ {0x04,0x15,0x0E,0x1F,0x0E,0x15,0x04},
    /* '+' */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    /* ',' */ {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    /* '-' */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
    /* '/' */ {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    /* '0' */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* '1' */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* '2' */ {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
    /* '3' */ {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    /* '4' */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* '5' */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* '6' */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* '7' */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* '8' */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* '9' */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* ':' */ {0x00,0x00,0x04,0x00,0x04,0x00,0x00},
    /* ';' */ {0x00,0x00,0x04,0x00,0x04,0x04,0x08},
    /* '<' */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    /* '=' */ {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    /* '>' */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    /* '?' */ {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    /* '@' */ {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    /* 'A' */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 'B' */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* 'C' */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* 'D' */ {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    /* 'E' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* 'F' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* 'G' */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    /* 'H' */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 'I' */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 'J' */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* 'K' */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* 'L' */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* 'M' */ {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    /* 'N' */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* 'O' */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 'P' */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* 'Q' */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* 'R' */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* 'S' */ {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
    /* 'T' */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* 'U' */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 'V' */ {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    /* 'W' */ {0x11,0x11,0x11,0x11,0x15,0x1B,0x11},
    /* 'X' */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* 'Y' */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* 'Z' */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* '[' */ {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    /* '\' */ {0x10,0x08,0x08,0x04,0x02,0x02,0x01},
    /* ']' */ {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    /* '^' */ {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    /* '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    /* '`' */ {0x08,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 'a' */ {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    /* 'b' */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},
    /* 'c' */ {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E},
    /* 'd' */ {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},
    /* 'e' */ {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    /* 'f' */ {0x06,0x08,0x1E,0x08,0x08,0x08,0x08},
    /* 'g' */ {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E},
    /* 'h' */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x11},
    /* 'i' */ {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    /* 'j' */ {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    /* 'k' */ {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    /* 'l' */ {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 'm' */ {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},
    /* 'n' */ {0x00,0x00,0x1E,0x11,0x11,0x11,0x11},
    /* 'o' */ {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    /* 'p' */ {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    /* 'q' */ {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01},
    /* 'r' */ {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    /* 's' */ {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
    /* 't' */ {0x08,0x08,0x1E,0x08,0x08,0x09,0x06},
    /* 'u' */ {0x00,0x00,0x11,0x11,0x11,0x11,0x0F},
    /* 'v' */ {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},
    /* 'w' */ {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    /* 'x' */ {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    /* 'y' */ {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    /* 'z' */ {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
    /* '{' */ {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    /* '|' */ {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    /* '}' */ {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    /* '~' */ {0x00,0x00,0x08,0x15,0x02,0x00,0x00},
};

/* ═══════════════════════════════════════════════════════════════ */
/*  Pixel and line drawing                                         */
/* ═══════════════════════════════════════════════════════════════ */

static void set_pixel(mcp40_t* mcp, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= MCP40_WIDTH || y < 0 || y >= MCP40_HEIGHT)
        return;

    /* Y is inverted: plotter Y=0 is bottom, framebuffer row 0 is top */
    int fy = (MCP40_HEIGHT - 1) - y;
    int offset = (fy * MCP40_WIDTH + x) * 3;
    mcp->framebuffer[offset]     = r;
    mcp->framebuffer[offset + 1] = g;
    mcp->framebuffer[offset + 2] = b;
}

/** Dash patterns: 1=draw, 0=skip. Period varies by type. */
static bool dash_visible(mcp40_linetype_t type, int step)
{
    switch (type) {
        case MCP40_LINE_SOLID: return true;
        case MCP40_LINE_DASH1: return (step % 8) < 5;       /* -----... */
        case MCP40_LINE_DASH2: return (step % 6) < 3;       /* ---... */
        case MCP40_LINE_DASH3: return (step % 4) < 2;       /* --.. */
        case MCP40_LINE_DASH4: return (step % 4) < 1;       /* -... */
        default: return true;
    }
}

/** Bresenham line drawing */
static void draw_line(mcp40_t* mcp, int x0, int y0, int x1, int y1)
{
    uint8_t r, g, b;
    mcp40_get_pen_rgb(mcp->color, &r, &g, &b);

    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int step = 0;

    for (;;) {
        if (dash_visible(mcp->line_type, step)) {
            set_pixel(mcp, x0, y0, r, g, b);
        }
        step++;

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }

    mcp->dirty = true;
    mcp->line_count++;
}

/** Draw a single character at (x, y) using the 5x7 font */
static void draw_char(mcp40_t* mcp, int x, int y, char ch)
{
    if (ch < 32 || ch > 126) return;
    int idx = ch - 32;

    uint8_t r, g, b;
    mcp40_get_pen_rgb(mcp->color, &r, &g, &b);

    int scale = mcp->char_size > 0 ? mcp->char_size : 1;

    for (int row = 0; row < 7; row++) {
        uint8_t bits = font_5x7[idx][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                /* Scale each font pixel */
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        set_pixel(mcp, x + col * scale + sx,
                                  y + (6 - row) * scale + sy, r, g, b);
                    }
                }
            }
        }
    }

    mcp->dirty = true;
    mcp->char_count++;
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Command execution                                              */
/* ═══════════════════════════════════════════════════════════════ */

/** Parse comma-separated integer pairs from param buffer */
static int parse_coords(const char* buf, int* coords, int max_coords)
{
    int count = 0;
    const char* p = buf;

    while (*p && count < max_coords) {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        coords[count++] = atoi(p);
        /* Skip to next separator */
        while (*p && *p != ',' && *p != ' ') p++;
    }

    return count;
}

static void execute_command(mcp40_t* mcp)
{
    char cmd = mcp->current_cmd;
    const char* params = mcp->param_buf;

    switch (cmd) {
        case 'I': /* Initialize */
            mcp40_reset(mcp);
            log_info("MCP-40: initialized");
            break;

        case 'H': /* Home */
            mcp->pen_x = 0;
            mcp->pen_y = 0;
            break;

        case 'D': { /* Draw (pen down) */
            int coords[64];
            int n = parse_coords(params, coords, 64);
            for (int i = 0; i + 1 < n; i += 2) {
                int x1 = coords[i];
                int y1 = coords[i + 1];
                /* Clamp to plotter bounds */
                if (x1 < 0) x1 = 0;
                if (x1 >= MCP40_WIDTH) x1 = MCP40_WIDTH - 1;
                if (y1 < 0) y1 = 0;
                if (y1 >= MCP40_HEIGHT) y1 = MCP40_HEIGHT - 1;

                draw_line(mcp, mcp->pen_x, mcp->pen_y, x1, y1);
                mcp->pen_x = x1;
                mcp->pen_y = y1;
            }
            break;
        }

        case 'M': { /* Move (pen up) */
            int coords[64];
            int n = parse_coords(params, coords, 64);
            for (int i = 0; i + 1 < n; i += 2) {
                int x1 = coords[i];
                int y1 = coords[i + 1];
                if (x1 < 0) x1 = 0;
                if (x1 >= MCP40_WIDTH) x1 = MCP40_WIDTH - 1;
                if (y1 < 0) y1 = 0;
                if (y1 >= MCP40_HEIGHT) y1 = MCP40_HEIGHT - 1;
                mcp->pen_x = x1;
                mcp->pen_y = y1;
            }
            break;
        }

        case 'J': { /* Select color */
            int c = atoi(params);
            if (c >= 0 && c <= 3) {
                mcp->color = (mcp40_color_t)c;
            }
            break;
        }

        case 'P': { /* Print character(s) */
            for (int i = 0; params[i]; i++) {
                draw_char(mcp, mcp->pen_x, mcp->pen_y, params[i]);
                /* Advance pen horizontally */
                mcp->pen_x += 6 * (mcp->char_size > 0 ? mcp->char_size : 1);
            }
            break;
        }

        case 'L': { /* Line type */
            int t = atoi(params);
            if (t >= 0 && t <= 4) {
                mcp->line_type = (mcp40_linetype_t)t;
            }
            break;
        }

        case 'Q': { /* Character size */
            int s = atoi(params);
            if (s >= 1 && s <= 63) {
                mcp->char_size = s;
            }
            break;
        }

        default:
            /* Unknown command — ignore */
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Public API                                                     */
/* ═══════════════════════════════════════════════════════════════ */

void mcp40_init(mcp40_t* mcp)
{
    memset(mcp, 0, sizeof(*mcp));
    /* White paper */
    memset(mcp->framebuffer, 0xFF, sizeof(mcp->framebuffer));
    mcp->pen_x = 0;
    mcp->pen_y = 0;
    mcp->color = MCP40_BLACK;
    mcp->line_type = MCP40_LINE_SOLID;
    mcp->char_size = 1;
    mcp->state = MCP40_IDLE;
    mcp->current_cmd = 0;
    mcp->param_len = 0;
    mcp->dirty = false;
    mcp->output_file = NULL;
}

void mcp40_reset(mcp40_t* mcp)
{
    const char* saved_file = mcp->output_file;
    /* Auto-export before reset if dirty */
    if (mcp->dirty && mcp->output_file) {
        mcp40_export_bmp(mcp, mcp->output_file);
    }
    mcp40_init(mcp);
    mcp->output_file = saved_file;
}

void mcp40_receive_byte(mcp40_t* mcp, uint8_t byte)
{
    char ch = (char)byte;

    /* CR or LF terminates current command */
    if (ch == '\r' || ch == '\n') {
        if (mcp->current_cmd) {
            mcp->param_buf[mcp->param_len] = '\0';
            execute_command(mcp);
            mcp->current_cmd = 0;
            mcp->param_len = 0;
            mcp->state = MCP40_IDLE;
        }
        return;
    }

    /* Is this a command letter? */
    bool is_cmd = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');

    /* P command: all chars after 'P' are text to print (not new commands) */
    bool in_print_mode = (mcp->state == MCP40_PARAM && mcp->current_cmd == 'P');

    if (is_cmd && mcp->state == MCP40_IDLE) {
        /* Start new command */
        mcp->current_cmd = (char)toupper(ch);
        mcp->param_len = 0;
        mcp->state = MCP40_PARAM;
    } else if (is_cmd && mcp->state == MCP40_PARAM && !in_print_mode) {
        /* New command letter while parsing params — execute previous first */
        mcp->param_buf[mcp->param_len] = '\0';
        execute_command(mcp);
        /* Start new command */
        mcp->current_cmd = (char)toupper(ch);
        mcp->param_len = 0;
        mcp->state = MCP40_PARAM;
    } else if (mcp->state == MCP40_PARAM) {
        /* Accumulate parameter characters */
        if (mcp->param_len < (int)sizeof(mcp->param_buf) - 1) {
            mcp->param_buf[mcp->param_len++] = ch;
        }
    }
    /* else: stray character in IDLE — ignore */
}

void mcp40_set_output(mcp40_t* mcp, const char* filename)
{
    mcp->output_file = filename;
}

/* ═══════════════════════════════════════════════════════════════ */
/*  BMP export                                                     */
/* ═══════════════════════════════════════════════════════════════ */

bool mcp40_export_bmp(const mcp40_t* mcp, const char* filename)
{
    if (!mcp || !filename) return false;

    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        log_error("MCP-40: cannot open '%s' for writing", filename);
        return false;
    }

    int w = MCP40_WIDTH;
    int h = MCP40_HEIGHT;
    unsigned int row_stride = (unsigned int)((w * 3 + 3) & ~3);
    uint32_t pixel_size = row_stride * (unsigned int)h;
    uint32_t file_size = 54 + pixel_size;

    /* BMP file header (14 bytes) */
    uint8_t bmp_header[14] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8),
        (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0
    };
    fwrite(bmp_header, 1, 14, fp);

    /* DIB header (40 bytes) */
    uint8_t dib_header[40] = {0};
    dib_header[0] = 40;
    dib_header[4]  = (uint8_t)(w);
    dib_header[5]  = (uint8_t)(w >> 8);
    dib_header[8]  = (uint8_t)(h);
    dib_header[9]  = (uint8_t)(h >> 8);
    dib_header[12] = 1;   /* planes */
    dib_header[14] = 24;  /* bits per pixel */
    dib_header[20] = (uint8_t)(pixel_size);
    dib_header[21] = (uint8_t)(pixel_size >> 8);
    dib_header[22] = (uint8_t)(pixel_size >> 16);
    dib_header[23] = (uint8_t)(pixel_size >> 24);
    fwrite(dib_header, 1, 40, fp);

    /* Pixel data (bottom-up, BGR) */
    uint8_t padding[3] = {0, 0, 0};
    unsigned int pad_bytes = row_stride - (unsigned int)(w * 3);

    /* BMP is bottom-up: row 0 = bottom of image.
     * Our framebuffer row 0 = top (Y inverted in set_pixel),
     * so BMP row 0 = framebuffer row h-1 = plotter Y=0 (bottom). */
    for (int row = h - 1; row >= 0; row--) {
        for (int col = 0; col < w; col++) {
            int offset = (row * w + col) * 3;
            uint8_t bgr[3] = {
                mcp->framebuffer[offset + 2],  /* B */
                mcp->framebuffer[offset + 1],  /* G */
                mcp->framebuffer[offset]       /* R */
            };
            fwrite(bgr, 1, 3, fp);
        }
        if (pad_bytes > 0) fwrite(padding, 1, pad_bytes, fp);
    }

    fclose(fp);
    log_info("MCP-40: exported to '%s' (%dx%d, %u lines, %u chars)",
             filename, w, h, mcp->line_count, mcp->char_count);
    return true;
}
