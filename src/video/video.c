/**
 * @file video.c
 * @brief ORIC-1 video system - text mode 40x28 + HIRES 240x200
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-23
 * @version 1.0.0-beta.5
 *
 * Video mode is tracked via vid_mode (persistent across frames),
 * set by serial attributes 24-31 in video data.
 * This matches Oricutron's ULA behavior.
 */

#include "video/video.h"
#include <string.h>

static const uint8_t palette[8][3] = {
    {0x00,0x00,0x00},{0xFF,0x00,0x00},{0x00,0xFF,0x00},{0xFF,0xFF,0x00},
    {0x00,0x00,0xFF},{0xFF,0x00,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},
};

bool video_init(video_t* vid) {
    memset(vid, 0, sizeof(video_t));
    vid->hires_mode = false;
    vid->need_refresh = true;
    vid->vid_mode = 0x02;  /* Powerup default: TEXT, PAL50 (same as Oricutron) */
    return true;
}

void video_cleanup(video_t* vid) { (void)vid; }

void video_reset(video_t* vid) {
    vid->hires_mode = false;
    vid->need_refresh = true;
    vid->vid_mode = 0x02;
    memset(vid->framebuffer, 0, sizeof(vid->framebuffer));
}

void video_set_mode(video_t* vid, bool hires) {
    vid->hires_mode = hires;
    vid->need_refresh = true;
}

void video_get_rgb(uint8_t oric_color, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t c = oric_color & 0x07;
    *r = palette[c][0]; *g = palette[c][1]; *b = palette[c][2];
}

static void set_pixel(video_t* vid, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= ORIC_SCREEN_W || y < 0 || y >= ORIC_SCREEN_H) return;
    int off = (y * ORIC_SCREEN_W + x) * 3;
    vid->framebuffer[off] = r; vid->framebuffer[off+1] = g; vid->framebuffer[off+2] = b;
}

/**
 * Get charset byte from RAM.
 *
 * ORIC charset addresses:
 *   TEXT mode:  $B400-$B7FF (standard charset, 128 chars x 8 bytes)
 *   HIRES mode: $9800-$9BFF (charset relocated because $B400 is in HIRES bitmap)
 */
static uint8_t get_charset_byte(video_t* vid, const uint8_t* mem, int char_idx, int row) {
    bool alt = (vid->text_attr & 0x01) != 0;
    if (vid->charset) {
        /* External charset (test/headless path): alt charset lives 128 chars after std */
        return vid->charset[(char_idx + (alt ? 128 : 0)) * 8 + row];
    }
    /* RAM charsets — standard at $B400/$9800, alternate at $B800/$9C00 */
    uint16_t base = vid->hires_mode
        ? (alt ? 0x9C00 : 0x9800)
        : (alt ? 0xB800 : 0xB400);
    return mem[base + char_idx * 8 + row];
}

/**
 * Decode a serial attribute (bits 6+5 both zero) and update ULA state.
 * Returns true if the attribute changed the video mode.
 */
static bool decode_attr(video_t* vid, uint8_t attr,
                        uint8_t* ink, uint8_t* paper, bool* inverse) {
    uint8_t val = attr & 0x1F;
    switch (val & 0x18) {
        case 0x00: *ink = val & 0x07; break;       /* 0-7: foreground (INK) */
        case 0x08:                                  /* 8-15: text attrs */
            /* bit 0 = alt charset, bit 1 = double height, bit 2 = blink */
            vid->text_attr = val & 0x07;
            break;
        case 0x10: *paper = val & 0x07; break;      /* 16-23: background (PAPER) */
        case 0x18:                                   /* 24-31: video mode */
            vid->vid_mode = val & 0x07;
            return true;
    }
    /* Also handle inverse for text-like rendering */
    if (val == 28 && inverse) *inverse = false;
    if (val == 29 && inverse) *inverse = true;
    return false;
}

/**
 * Render a single pixel block (6 pixels) for a HIRES data byte.
 */
static void render_hires_block(video_t* vid, int x, int y,
                               uint8_t byte, uint8_t ink, uint8_t paper) {
    bool inv = (byte & 0x80) != 0;
    uint8_t fg = inv ? paper : ink;
    uint8_t bg = inv ? ink : paper;
    uint8_t ir, ig, ib, pr, pg, pb;
    video_get_rgb(fg, &ir, &ig, &ib);
    video_get_rgb(bg, &pr, &pg, &pb);
    for (int bx = 5; bx >= 0; bx--) {
        if (byte & (1 << bx))
            set_pixel(vid, x + (5 - bx), y, ir, ig, ib);
        else
            set_pixel(vid, x + (5 - bx), y, pr, pg, pb);
    }
}

/**
 * Render an attribute block (6 pixels wide, filled with paper color).
 * For text mode, renders 6x8; for HIRES, renders 6x1.
 */
static void render_attr_block(video_t* vid, int x, int y,
                              uint8_t paper, int height) {
    uint8_t pr, pg, pb;
    video_get_rgb(paper, &pr, &pg, &pb);
    for (int cy = 0; cy < height; cy++)
        for (int bx = 0; bx < 6; bx++)
            set_pixel(vid, x + bx, y + cy, pr, pg, pb);
}

/**
 * Render the full frame, line by line.
 *
 * ULA behavior (matching Oricutron):
 * - vid_mode persists across frames, set by serial attributes 24-31
 * - At start of each scanline: reset ink=white, paper=black
 * - Lines 0-199: if vid_mode & 4 → HIRES (read $A000+y*40), else TEXT ($BB80)
 * - Lines 200-223: always TEXT from $BB80 (rows 25-27)
 * - Serial attributes can change vid_mode mid-frame
 */
/* ═══════════════════════════════════════════════════════════════════════
 *  Main render entry point
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * Render one scanline (y in [0, 223]) by sampling the current memory state.
 *
 * Lines 0-199: HIRES or TEXT depending on vid_mode (persistent across
 * scanlines, per real Oric ULA). Mid-line attribute bytes can flip the
 * read source between $A000+ (HIRES) and $BB80+ (TEXT), so the base
 * address is recomputed each column from the current vid_mode.
 *
 * Lines 200-223: always TEXT from $BB80 (rows 25-27 of the 28-row text
 * screen). These rows can also contain attribute bytes that change
 * vid_mode for subsequent scanlines.
 *
 * This is the "hardware-accurate" rendering path: called once per PAL
 * scanline boundary (every PAL_CYCLES_PER_LINE = 64 CPU cycles) from
 * the emulator main loop, so each scanline sees the memory state at
 * that exact CPU cycle. Mimics Oricutron's `ula_doraster`.
 */
/* Effective char-row index applying double-height, matching the Oricutron/
 * real ULA model: double height uses (y>>1)&7, i.e. the absolute char-row
 * parity (row&1), NOT a content-driven phase. With y = row*8 + chline:
 *   (y>>1)&7 == (chline>>1) + ((row&1) ? 4 : 0)
 * Even rows show the top half (glyph rows 0-3), odd rows the bottom half
 * (glyph rows 4-7). This reproduces the "double-height must start on an even
 * row or it is garbled" hardware quirk. */
static int effective_chline(video_t* vid, int chline, int row) {
    if (vid->text_attr & 0x02)
        return (chline >> 1) + ((row & 1) ? 4 : 0);
    return chline;
}

/* Whether blink-attr is currently in its "hidden/inverted" phase. */
static bool blink_phase_on(video_t* vid) {
    return (vid->text_attr & 0x04) && (vid->frame_counter & 0x10);
}

void video_render_scanline(video_t* vid, const uint8_t* memory, int y) {
    if (!memory) return;
    if (y < 0 || y >= 224) return;

    if (y == 0)
        vid->frame_counter++;

    /* ULA resets attributes at start of every scanline. */
    vid->text_attr = 0;

    if (y < 200) {
        uint8_t ink = ORIC_WHITE, paper = ORIC_BLACK;
        int row = y / 8;
        int chline = y & 7;

        for (int col = 0; col < 40; col++) {
            bool hires = (vid->vid_mode & 0x04) != 0;
            uint16_t base = hires ? (0xA000 + y * 40) : (0xBB80 + row * 40);
            uint8_t byte = memory[base + col];

            if ((byte & 0x60) == 0) {
                bool inverse = false;
                decode_attr(vid, byte, &ink, &paper, &inverse);
                render_attr_block(vid, col * 6, y, paper, 1);
            } else if (hires) {
                render_hires_block(vid, col * 6, y, byte, ink, paper);
            } else {
                bool inverse = false;
                uint8_t fg = inverse ? paper : ink;
                uint8_t bg = inverse ? ink : paper;
                uint8_t ir, ig, ib, pr, pg, pb;
                video_get_rgb(fg, &ir, &ig, &ib);
                video_get_rgb(bg, &pr, &pg, &pb);
                int erow = effective_chline(vid, chline, row);
                uint8_t bits = get_charset_byte(vid, memory, byte & 0x7F, erow);
                bool char_inv = (byte & 0x80) != 0;
                if (blink_phase_on(vid)) char_inv = !char_inv;
                for (int bx = 5; bx >= 0; bx--) {
                    bool on = (bits & (1 << bx)) != 0;
                    if (char_inv) on = !on;
                    if (on) set_pixel(vid, col * 6 + (5 - bx), y, ir, ig, ib);
                    else    set_pixel(vid, col * 6 + (5 - bx), y, pr, pg, pb);
                }
            }
        }

        if (y == 199) vid->hires_mode = (vid->vid_mode & 0x04) != 0;
    } else {
        /* Lines 200-223: always TEXT from $BB80 (rows 25-27) */
        int row = 25 + (y - 200) / 8;
        int chline = (y - 200) & 7;
        uint8_t ink = ORIC_WHITE, paper = ORIC_BLACK;

        for (int col = 0; col < 40; col++) {
            uint8_t byte = memory[0xBB80 + row * 40 + col];
            if ((byte & 0x60) == 0) {
                bool inverse = false;
                decode_attr(vid, byte, &ink, &paper, &inverse);
                render_attr_block(vid, col * 6, y, paper, 1);
            } else {
                bool inverse = false;
                uint8_t fg = inverse ? paper : ink;
                uint8_t bg = inverse ? ink : paper;
                uint8_t ir, ig, ib, pr, pg, pb;
                video_get_rgb(fg, &ir, &ig, &ib);
                video_get_rgb(bg, &pr, &pg, &pb);
                int erow = effective_chline(vid, chline, row);
                uint8_t bits = get_charset_byte(vid, memory, byte & 0x7F, erow);
                bool char_inv = (byte & 0x80) != 0;
                if (blink_phase_on(vid)) char_inv = !char_inv;
                for (int bx = 5; bx >= 0; bx--) {
                    bool on = (bits & (1 << bx)) != 0;
                    if (char_inv) on = !on;
                    if (on) set_pixel(vid, col * 6 + (5 - bx), y, ir, ig, ib);
                    else    set_pixel(vid, col * 6 + (5 - bx), y, pr, pg, pb);
                }
            }
        }

        if (y == 223) vid->need_refresh = false;
    }
}

void video_render_frame(video_t* vid, const uint8_t* memory) {
    if (!memory) return;
    /* Fallback path: render the whole frame in one pass against the current
     * memory snapshot. Used when the main loop hasn't ticked per-scanline
     * (e.g. headless --screenshot at exit) or as a last resort. */
    for (int y = 0; y < 224; y++) video_render_scanline(vid, memory, y);
}
