/**
 * @file video.h
 * @brief ORIC-1 video system interface
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>
#include <stdbool.h>

#define ORIC_SCREEN_W   240
#define ORIC_SCREEN_H   224
/* Maximum framebuffer dimensions across all ULA profiles. The OCULA
 * profile (RP2350-based ULA replacement, forum.defence-force.org t=2709)
 * reserves room for future 80-column text (480 px) and doubled vertical
 * resolution. The framebuffer is allocated at this capacity; the active
 * resolution is native_w x native_h (240x224 for both profiles until the
 * OCULA extended modes land). */
#define OCULA_MAX_W     480
#define OCULA_MAX_H     448
#define ORIC_TEXT_COLS   40
#define ORIC_TEXT_ROWS   28
#define ORIC_HIRES_W    240
#define ORIC_HIRES_H    200
#define ORIC_CHAR_W     6
#define ORIC_CHAR_H     8
#define ORIC_FPS         50

/* OCULA 80-column text mode (extended serial attribute 25, OCULA profile
 * only — see docs/ocula_extensions.md). 80 chars x 6 px = 480 px wide.
 * Screen RAM moves to $A000 (80 bytes/row x 28 rows = $A000-$A8BF), the
 * region HIRES uses, so the standard ($B400) and alternate ($B800) TEXT
 * charsets remain available. */
#define OCULA_80COL_COLS  80
#define OCULA_80COL_BASE  0xA000

/* OCULA extended HIRES 320x200 bicolor (extended serial attribute 29/31).
 * Same memory footprint as stock HIRES ($A000-$BF3F, 40 bytes/row x 200)
 * but all 8 bits of every byte are pixels (no serial attributes, no
 * invert bit) — 320 px wide, ink/paper fixed to palette entries 7/0.
 * Lines 200-223 stay standard text rows (in-band escape hatch: a mode
 * attribute there switches back). 480x200 was considered but 16000
 * bytes do not fit the $A000-$BFFF window. */
#define OCULA_EXTHIRES_W     320
#define OCULA_EXTHIRES_BASE  0xA000

/* OCULA redefinable palette: 8 RGB332 entries at $BFE0-$BFE7 (the 32
 * bytes above the text screen, never scanned by a stock ULA), armed by
 * the magic bytes 'O','C' at $BFE8-$BFE9. Re-read at every frame start;
 * without the magic the standard Oric palette applies. */
#define OCULA_PAL_BASE   0xBFE0
#define OCULA_PAL_MAGIC  0xBFE8

/* ORIC colors */
#define ORIC_BLACK   0
#define ORIC_RED     1
#define ORIC_GREEN   2
#define ORIC_YELLOW  3
#define ORIC_BLUE    4
#define ORIC_MAGENTA 5
#define ORIC_CYAN    6
#define ORIC_WHITE   7

/* ULA profile: which chip the video subsystem emulates.
 * HCS10017 is the stock Oric ULA (default, strictly faithful).
 * OCULA is the RP2350-based replacement project — identical to the stock
 * ULA for now; extended modes (80-col text, redefinable palette, hi-res
 * mono) will only ever be reachable under this profile. */
typedef enum {
    ULA_PROFILE_HCS10017 = 0,
    ULA_PROFILE_OCULA    = 1,
} ula_profile_t;

typedef struct video_s {
    uint8_t framebuffer[OCULA_MAX_W * OCULA_MAX_H * 3]; /* RGB888, native_w x native_h used */
    ula_profile_t ula_profile;
    int native_w;           /* Active framebuffer width (stride) in pixels */
    int native_h;           /* Active framebuffer height in pixels */
    bool hires_mode;
    bool need_refresh;
    uint8_t* screen_ram;    /* Pointer into memory $BB80 (text) or $A000 (hires) */
    uint8_t* charset;       /* Character set ROM */
    uint8_t vid_mode;       /* ULA video mode (persistent, set by serial attrs 24-31).
                             * Bit 2: HIRES when set. Initialized to 2 (TEXT/PAL50).
                             * Bit 0 is a don't-care on the stock HCS 10017; under
                             * the OCULA profile it selects 80-column text when
                             * HIRES (bit 2) is clear. */

    /* OCULA 80-column latch: evaluated once per frame (scanline 0) from
     * ula_profile + vid_mode, so the framebuffer stride stays constant
     * for a whole frame. Mid-frame attribute changes take effect at the
     * next frame. */
    bool ocula_80col;

    /* OCULA extended-HIRES latch (attr 29/31): same frame-start
     * evaluation as ocula_80col. 80-col (bit 2 clear) and ext-HIRES
     * (bit 2 set) are mutually exclusive by construction. */
    bool ocula_exthires;

    /* Active palette, RGB888 per Oric color 0-7. Standard palette by
     * default; under OCULA, redefinable per frame from OCULA_PAL_BASE
     * when the OCULA_PAL_MAGIC bytes are armed. */
    uint8_t pal_rgb[8][3];

    /* OCULA-GPU hardware scroll (SCROLL command, étape 5): applied to
     * the extended-HIRES fetch only, wrap modulo 320/200. 0,0 = off. */
    uint8_t ocula_scroll_x;
    uint8_t ocula_scroll_y;

    /* Serial-attribute group 0x08-0x0F (text attrs).
     * bit 0 = alt charset, bit 1 = double height, bit 2 = blink.
     * Reset to 0 at start of every scanline; latched per-column. */
    uint8_t text_attr;

    /* Per-row double-height phase: 0 = top half of doubled glyph,
     * 1 = bottom half. Toggled at the end of each text row where
     * double was active; reset when a row has no double attribute. */
    uint8_t dbl_phase;
    bool dbl_was_active;    /* Tracks whether the previous text row had DBL set */

    /* Frame counter for blink (toggles every ~16 frames, ~3 Hz). */
    uint32_t frame_counter;
} video_t;

bool video_init(video_t* vid);
void video_cleanup(video_t* vid);
void video_reset(video_t* vid);
void video_set_mode(video_t* vid, bool hires);
void video_render_frame(video_t* vid, const uint8_t* memory);
void video_render_scanline(video_t* vid, const uint8_t* memory, int y);
void video_get_rgb(uint8_t oric_color, uint8_t* r, uint8_t* g, uint8_t* b);

/* ULA profile management */
void video_set_profile(video_t* vid, ula_profile_t profile);
ula_profile_t video_get_profile(const video_t* vid);
const char* video_profile_name(ula_profile_t profile);
int video_profile_parse(const char* name); /* returns ula_profile_t or -1 */

#endif
