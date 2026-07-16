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
#include "io/ula_ng.h"   /* composition sprites §5.7 (ng_dev) */
#include <string.h>
#include <strings.h>

static const uint8_t palette[8][3] = {
    {0x00,0x00,0x00},{0xFF,0x00,0x00},{0x00,0xFF,0x00},{0xFF,0xFF,0x00},
    {0x00,0x00,0xFF},{0xFF,0x00,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},
};

/* Active resolution: follows the OCULA mode latches; the stock profile
 * and OCULA-in-standard-mode both render 240x224. */
static void apply_profile_resolution(video_t* vid) {
    if (vid->ng_text80)           vid->native_w = OCULA_MAX_W;      /* §5.8 : 80 col × 6 = 480 */
    else if (vid->ng_chunky)      vid->native_w = OCULA_EXTHIRES_W; /* §5.8 : 160 chunky × 2 = 320 */
    else if (vid->ocula_80col)    vid->native_w = OCULA_MAX_W;
    else if (vid->ocula_exthires) vid->native_w = OCULA_EXTHIRES_W;
    else                          vid->native_w = ORIC_SCREEN_W;
    vid->native_h = ORIC_SCREEN_H;
}

static void palette_reset(video_t* vid) {
    memcpy(vid->pal_rgb, palette, sizeof(vid->pal_rgb));
}

/* Expand one RGB332 byte (RRRGGGBB) to RGB888. Shared by the redefinable
 * palette and the border register — both live in the gated $BFE0-$BFFF
 * block and so must decode identically. */
static void rgb332_to_rgb888(uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = (uint8_t)((((v >> 5) & 0x07) * 255) / 7);
    *g = (uint8_t)((((v >> 2) & 0x07) * 255) / 7);
    *b = (uint8_t)(((v & 0x03) * 255) / 3);
}

/* True when the gated $BFE0-$BFFF block is live: OCULA profile, unlocked,
 * and the 'O','C' magic armed at OCULA_PAL_MAGIC. Opt-in (sprint 45):
 * otherwise the block is plain storage (some games keep data there — cf.
 * Dbug, forum t=2709), so neither the palette nor the border react. */
static bool ocula_block_armed(const video_t* vid, const uint8_t* memory) {
    return vid->ula_profile == ULA_PROFILE_OCULA && vid->ocula_unlocked &&
           memory[OCULA_PAL_MAGIC] == 'O' && memory[OCULA_PAL_MAGIC + 1] == 'C';
}

/* True when the OCULA write-only register file is the active source for
 * palette + border (sprint 66): a register was written since unlock. Live
 * read through the pointer wired from memory_t; NULL in the bare unit-test
 * path. When armed, the register file overrides the in-band $BFE0-$BFFF
 * block during the transition to sodiumlb's register scheme. */
static bool ocula_regs_active(const video_t* vid) {
    return vid->ula_profile == ULA_PROFILE_OCULA &&   /* never on a stock ULA */
           vid->ocula_regs_armed && *vid->ocula_regs_armed &&
           vid->ocula_reg_pal && vid->ocula_reg_border;
}

/* Re-evaluate the redefinable palette at scanline start. Source order:
 * (1) the write-only register file if armed (sprint 66), else (2) the in-band
 * $BFE0-$BFE7 block if magic-armed + unlocked, else (3) the standard palette. */
static void palette_latch(video_t* vid, const uint8_t* memory) {
    /* ULA-NG palette-indirection (§5.1) : la LUT NG (déjà RGB888) prime quand
     * les extensions palette sont actives. Entrées 0-7 (mode standard). */
    if (vid->ng_active && *vid->ng_active && vid->ng_pal) {
        for (int i = 0; i < 8; i++) {
            vid->pal_rgb[i][0] = vid->ng_pal[i][0];
            vid->pal_rgb[i][1] = vid->ng_pal[i][1];
            vid->pal_rgb[i][2] = vid->ng_pal[i][2];
        }
    } else if (ocula_regs_active(vid)) {
        for (int i = 0; i < 8; i++) {
            rgb332_to_rgb888(vid->ocula_reg_pal[i],
                             &vid->pal_rgb[i][0], &vid->pal_rgb[i][1],
                             &vid->pal_rgb[i][2]);
        }
    } else if (ocula_block_armed(vid, memory)) {
        for (int i = 0; i < 8; i++) {
            rgb332_to_rgb888(memory[OCULA_PAL_BASE + i],
                             &vid->pal_rgb[i][0], &vid->pal_rgb[i][1],
                             &vid->pal_rgb[i][2]);
        }
    } else {
        palette_reset(vid);
    }
}

/* Latch the border colour for scanline y from OCULA_BORDER_REG ($BFEA),
 * RGB332, under the same gating as the palette. Black ($00 / disarmed /
 * stock ULA) reproduces the standard Oric border. Re-read every scanline,
 * so rewriting $BFEA between lines yields border raster bars (sprint 64,
 * cf. Dbug forum t=2709). The framebuffer has no overscan band yet — this
 * only fills the per-line model exposed by video_get_border_rgb(). */
static void border_latch(video_t* vid, const uint8_t* memory, int y) {
    if (y < 0 || y >= OCULA_MAX_H) return;
    if (ocula_regs_active(vid)) {
        rgb332_to_rgb888(*vid->ocula_reg_border,
                         &vid->ocula_border[y][0], &vid->ocula_border[y][1],
                         &vid->ocula_border[y][2]);
    } else if (ocula_block_armed(vid, memory)) {
        rgb332_to_rgb888(memory[OCULA_BORDER_REG],
                         &vid->ocula_border[y][0], &vid->ocula_border[y][1],
                         &vid->ocula_border[y][2]);
    } else {
        vid->ocula_border[y][0] = 0;
        vid->ocula_border[y][1] = 0;
        vid->ocula_border[y][2] = 0;
    }
}

/* Palette-aware color lookup used by all rendering paths. */
static void get_rgb(const video_t* vid, uint8_t oric_color,
                    uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t c = oric_color & 0x07;
    *r = vid->pal_rgb[c][0]; *g = vid->pal_rgb[c][1]; *b = vid->pal_rgb[c][2];
}

bool video_init(video_t* vid) {
    memset(vid, 0, sizeof(video_t));
    vid->ula_profile = ULA_PROFILE_HCS10017;
    apply_profile_resolution(vid);
    palette_reset(vid);
    vid->hires_mode = false;
    vid->need_refresh = true;
    vid->vid_mode = 0x02;  /* Powerup default: TEXT, PAL50 (same as Oricutron) */
    return true;
}

void video_cleanup(video_t* vid) { (void)vid; }

void video_reset(video_t* vid) {
    /* ula_profile survives reset: the profile models which physical chip
     * is socketed, not a runtime mode. */
    vid->ocula_80col = false;
    vid->ocula_exthires = false;
    vid->ocula_scroll_x = 0;
    vid->ocula_scroll_y = 0;
    apply_profile_resolution(vid);
    palette_reset(vid);
    vid->hires_mode = false;
    vid->need_refresh = true;
    vid->vid_mode = 0x02;
    memset(vid->framebuffer, 0, sizeof(vid->framebuffer));
    memset(vid->ocula_border, 0, sizeof(vid->ocula_border));
}

void video_set_profile(video_t* vid, ula_profile_t profile) {
    if (vid->ula_profile == profile) return;
    vid->ula_profile = profile;
    vid->ocula_80col = false;
    vid->ocula_exthires = false;
    vid->ocula_scroll_x = 0;
    vid->ocula_scroll_y = 0;
    apply_profile_resolution(vid);
    palette_reset(vid);
    memset(vid->framebuffer, 0, sizeof(vid->framebuffer));
    memset(vid->ocula_border, 0, sizeof(vid->ocula_border));
    vid->need_refresh = true;
}

ula_profile_t video_get_profile(const video_t* vid) {
    return vid->ula_profile;
}

const char* video_profile_name(ula_profile_t profile) {
    return profile == ULA_PROFILE_OCULA ? "ocula" : "ula";
}

int video_profile_parse(const char* name) {
    if (!name) return -1;
    if (strcasecmp(name, "ula") == 0 ||
        strcasecmp(name, "hcs10017") == 0) return ULA_PROFILE_HCS10017;
    if (strcasecmp(name, "ocula") == 0) return ULA_PROFILE_OCULA;
    return -1;
}

void video_set_mode(video_t* vid, bool hires) {
    vid->hires_mode = hires;
    vid->need_refresh = true;
}

void video_get_rgb(uint8_t oric_color, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t c = oric_color & 0x07;
    *r = palette[c][0]; *g = palette[c][1]; *b = palette[c][2];
}

void video_get_border_rgb(const video_t* vid, int y,
                          uint8_t* r, uint8_t* g, uint8_t* b) {
    if (y < 0 || y >= OCULA_MAX_H) { *r = *g = *b = 0; return; }
    *r = vid->ocula_border[y][0];
    *g = vid->ocula_border[y][1];
    *b = vid->ocula_border[y][2];
}

int video_bordered_w(const video_t* vid) {
    return vid->native_w + 2 * OCULA_BORDER_W;
}

int video_bordered_h(const video_t* vid) {
    return vid->native_h + 2 * OCULA_BORDER_H;
}

void video_compose_bordered(const video_t* vid, uint8_t* out, int* w, int* h) {
    int aw = vid->native_w, ah = vid->native_h;
    int tw = aw + 2 * OCULA_BORDER_W;
    int th = ah + 2 * OCULA_BORDER_H;

    for (int ty = 0; ty < th; ty++) {
        /* Active line under this output row (negative / past-end in the
         * top & bottom bands, where we clamp to the first/last line). */
        int ay = ty - OCULA_BORDER_H;
        int cy = ay < 0 ? 0 : (ay >= ah ? ah - 1 : ay);
        const uint8_t* bc = vid->ocula_border[cy];

        uint8_t* row = out + (size_t)ty * tw * 3;
        for (int tx = 0; tx < tw; tx++) {
            row[tx * 3 + 0] = bc[0];
            row[tx * 3 + 1] = bc[1];
            row[tx * 3 + 2] = bc[2];
        }
        /* Overlay the active scanline inside its left/right border. */
        if (ay >= 0 && ay < ah) {
            const uint8_t* src = vid->framebuffer + (size_t)ay * aw * 3;
            memcpy(row + (size_t)OCULA_BORDER_W * 3, src, (size_t)aw * 3);
        }
    }
    if (w) *w = tw;
    if (h) *h = th;
}

static void set_pixel(video_t* vid, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= vid->native_w || y < 0 || y >= vid->native_h) return;
    int off = (y * vid->native_w + x) * 3;
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
 *
 * Bit 7 (inverse) does NOT swap ink/paper — the real ULA (and Oricutron)
 * complements both colours (XOR 7): fg -> fg^7, bg -> bg^7. This only
 * differs from a plain swap when ink and paper are not each other's
 * complement (e.g. blue ink on black paper: swap gives black-on-blue,
 * but the hardware gives yellow-on-white). AIC images rely on this.
 */
static void render_hires_block(video_t* vid, int x, int y,
                               uint8_t byte, uint8_t ink, uint8_t paper) {
    bool inv = (byte & 0x80) != 0;
    uint8_t fg = inv ? (uint8_t)(ink ^ 0x07) : ink;
    uint8_t bg = inv ? (uint8_t)(paper ^ 0x07) : paper;
    uint8_t ir, ig, ib, pr, pg, pb;
    get_rgb(vid, fg, &ir, &ig, &ib);
    get_rgb(vid, bg, &pr, &pg, &pb);
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
 *
 * The ULA draws the attribute cell itself in the (new) background colour.
 * If the attribute byte has bit 7 set (inverse), the background is
 * complemented (XOR 7), matching Oricutron's ula_render_block(inverted,
 * data=0) which uses bg^7 for every dot.
 */
static void render_attr_block(video_t* vid, int x, int y,
                              uint8_t paper, int height, bool inverse) {
    uint8_t bg = inverse ? (uint8_t)(paper ^ 0x07) : paper;
    uint8_t pr, pg, pb;
    get_rgb(vid, bg, &pr, &pg, &pb);
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

/**
 * Render one 80-column text scanline (OCULA profile, extended serial
 * attribute 25). All 28 rows (lines 0-223) read from $A000 + row*80;
 * there is no HIRES mixing — a HIRES attribute in the stream still
 * updates vid_mode, which is re-latched at the next frame start.
 * Serial attributes (ink/paper/text attrs) work per column as in the
 * stock 40-column mode.
 */
static void render_80col_scanline(video_t* vid, const uint8_t* memory, int y) {
    int row = y / 8;
    int chline = y & 7;
    uint8_t ink = ORIC_WHITE, paper = ORIC_BLACK;

    for (int col = 0; col < OCULA_80COL_COLS; col++) {
        uint8_t byte = memory[OCULA_80COL_BASE + row * OCULA_80COL_COLS + col];
        if ((byte & 0x60) == 0) {
            bool inverse = false;
            decode_attr(vid, byte, &ink, &paper, &inverse);
            render_attr_block(vid, col * 6, y, paper, 1, (byte & 0x80) != 0);
        } else {
            bool char_inv = (byte & 0x80) != 0;
            if (blink_phase_on(vid)) char_inv = !char_inv;
            /* Inverse complements ink/paper (XOR 7), it does not swap them. */
            uint8_t fg = char_inv ? (uint8_t)(ink ^ 0x07) : ink;
            uint8_t bg = char_inv ? (uint8_t)(paper ^ 0x07) : paper;
            uint8_t ir, ig, ib, pr, pg, pb;
            get_rgb(vid, fg, &ir, &ig, &ib);
            get_rgb(vid, bg, &pr, &pg, &pb);
            int erow = effective_chline(vid, chline, row);
            uint8_t bits = get_charset_byte(vid, memory, byte & 0x7F, erow);
            for (int bx = 5; bx >= 0; bx--) {
                bool on = (bits & (1 << bx)) != 0;
                if (on) set_pixel(vid, col * 6 + (5 - bx), y, ir, ig, ib);
                else    set_pixel(vid, col * 6 + (5 - bx), y, pr, pg, pb);
            }
        }
    }

    if (y == 223) vid->need_refresh = false;
}

/**
 * Render one OCULA extended-HIRES scanline (attr 29/31): 320x200
 * bicolor bitmap at $A000, 40 bytes/row, all 8 bits are pixels (MSB
 * leftmost) — no serial attributes, no invert bit. Colors are palette
 * entries 7 (ink) and 0 (paper), redefinable via the OCULA palette.
 */
static void render_exthires_scanline(video_t* vid, const uint8_t* memory, int y) {
    uint8_t ir, ig, ib, pr, pg, pb;
    get_rgb(vid, ORIC_WHITE, &ir, &ig, &ib);
    get_rgb(vid, ORIC_BLACK, &pr, &pg, &pb);
    /* OCULA-GPU hardware scroll: source fetch shifted by (dx, dy),
     * wrapping modulo 320x200. Per-pixel fetch keeps the bit shifting
     * between source bytes trivial. */
    int src_y = (y + vid->ocula_scroll_y) % 200;
    int dx = vid->ocula_scroll_x;
    for (int x = 0; x < OCULA_EXTHIRES_W; x++) {
        int sx = (x + dx) % OCULA_EXTHIRES_W;
        uint8_t byte = memory[OCULA_EXTHIRES_BASE + src_y * 40 + (sx >> 3)];
        if (byte & (0x80 >> (sx & 7)))
            set_pixel(vid, x, y, ir, ig, ib);
        else
            set_pixel(vid, x, y, pr, pg, pb);
    }
}

/* Rangées de statut 200-223 : toujours TEXT depuis $BB80 (rows 25-27). Factorisé
 * pour être réutilisé par le mode chunky NG (§5.8), dont seules les lignes
 * 0-199 sont chunky. */
static void render_bottom_text_scanline(video_t* vid, const uint8_t* memory, int y) {
    int row = 25 + (y - 200) / 8;
    int chline = (y - 200) & 7;
    uint8_t ink = ORIC_WHITE, paper = ORIC_BLACK;

    for (int col = 0; col < 40; col++) {
        uint8_t byte = memory[0xBB80 + row * 40 + col];
        if ((byte & 0x60) == 0) {
            bool inverse = false;
            decode_attr(vid, byte, &ink, &paper, &inverse);
            render_attr_block(vid, col * 6, y, paper, 1, (byte & 0x80) != 0);
        } else {
            bool char_inv = (byte & 0x80) != 0;
            if (blink_phase_on(vid)) char_inv = !char_inv;
            uint8_t fg = char_inv ? (uint8_t)(ink ^ 0x07) : ink;
            uint8_t bg = char_inv ? (uint8_t)(paper ^ 0x07) : paper;
            uint8_t ir, ig, ib, pr, pg, pb;
            get_rgb(vid, fg, &ir, &ig, &ib);
            get_rgb(vid, bg, &pr, &pg, &pb);
            int erow = effective_chline(vid, chline, row);
            uint8_t bits = get_charset_byte(vid, memory, byte & 0x7F, erow);
            for (int bx = 5; bx >= 0; bx--) {
                bool on = (bits & (1 << bx)) != 0;
                if (on) set_pixel(vid, col * 6 + (5 - bx), y, ir, ig, ib);
                else    set_pixel(vid, col * 6 + (5 - bx), y, pr, pg, pb);
            }
        }
    }
}

/* ULA-NG chunky 4bpp (§5.8) : 160 pixels/rangée × 200, chacun 4 bits = index
 * dans la LUT palette NG (16 entrées, RGB888). Données : NG_SCRSTART (défaut
 * $A000), 80 octets/rangée (2 quartets/octet : quartet haut = pixel gauche).
 * Chaque pixel chunky occupe 2 pixels framebuffer (160×2 = 320). */
static void render_ng_chunky_scanline(video_t* vid, const uint8_t* memory, int y) {
    uint16_t base = 0xA000;
    if (vid->ng_scrstart && *vid->ng_scrstart) base = *vid->ng_scrstart;
    base = (uint16_t)(base + y * 80);
    for (int col = 0; col < 80; col++) {
        uint8_t byte = memory[(uint16_t)(base + col)];
        for (int half = 0; half < 2; half++) {
            uint8_t idx = half == 0 ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
            uint8_t r, g, b;
            if (vid->ng_pal) { r = vid->ng_pal[idx][0]; g = vid->ng_pal[idx][1]; b = vid->ng_pal[idx][2]; }
            else             { get_rgb(vid, idx, &r, &g, &b); }   /* fallback 8 couleurs */
            int x0 = (col * 2 + half) * 2;
            set_pixel(vid, x0,     y, r, g, b);
            set_pixel(vid, x0 + 1, y, r, g, b);
        }
    }
}

/* ULA-NG texte 80 colonnes (§5.8) : 80 caractères × 6 px = 480. Charset RAM
 * (redéfinissable, $B400/$B800 via get_charset_byte). Données : NG_SCRSTART
 * (défaut $A000), 80 octets/rangée. Attributs série et couleurs (LUT NG 0-7)
 * comme en texte standard. */
static void render_ng_text80_scanline(video_t* vid, const uint8_t* memory, int y) {
    uint16_t base = 0xA000;
    if (vid->ng_scrstart && *vid->ng_scrstart) base = *vid->ng_scrstart;
    int row = y / 8;
    int chline = y & 7;
    uint8_t ink = ORIC_WHITE, paper = ORIC_BLACK;

    for (int col = 0; col < 80; col++) {
        uint8_t byte = memory[(uint16_t)(base + row * 80 + col)];
        if ((byte & 0x60) == 0) {
            bool inverse = false;
            decode_attr(vid, byte, &ink, &paper, &inverse);
            render_attr_block(vid, col * 6, y, paper, 1, (byte & 0x80) != 0);
        } else {
            bool char_inv = (byte & 0x80) != 0;
            if (blink_phase_on(vid)) char_inv = !char_inv;
            uint8_t fg = char_inv ? (uint8_t)(ink ^ 0x07) : ink;
            uint8_t bg = char_inv ? (uint8_t)(paper ^ 0x07) : paper;
            uint8_t ir, ig, ib, pr, pg, pb;
            get_rgb(vid, fg, &ir, &ig, &ib);
            get_rgb(vid, bg, &pr, &pg, &pb);
            int erow = effective_chline(vid, chline, row);
            uint8_t bits = get_charset_byte(vid, memory, byte & 0x7F, erow);
            for (int bx = 5; bx >= 0; bx--) {
                bool on = (bits & (1 << bx)) != 0;
                if (on) set_pixel(vid, col * 6 + (5 - bx), y, ir, ig, ib);
                else    set_pixel(vid, col * 6 + (5 - bx), y, pr, pg, pb);
            }
        }
    }
}

void video_render_scanline(video_t* vid, const uint8_t* memory, int y) {
    if (!memory) return;
    if (y < 0 || y >= 224) return;

    if (y == 0) {
        vid->frame_counter++;

        /* OCULA frame-start latches (stride stays stable for a whole
         * frame): 80-col = bit 0 with HIRES clear; ext-HIRES = bit 0
         * with HIRES set. ocula_80col_forced (--ocula-80col-basic) bypasses
         * the vid_mode check so BASIC's standard attrs don't drop the mode.
         * (The redefinable palette is re-read per scanline below.)
         *
         * Opt-in (sprint 45): the extended modes only react to the
         * serial attributes 25/27/29/31 once the OCULA has been unlocked.
         * Until then attrs 25/27 keep their stock Oric meaning, so games
         * that use them interchangeably (cf. Dbug, forum t=2709) are not
         * disturbed. The forced BASIC-mirror mode is an explicit CLI
         * opt-in and stays independent of the unlock. */
        bool ext_enabled = (vid->ula_profile == ULA_PROFILE_OCULA) &&
                           vid->ocula_unlocked;
        bool want_80col = vid->ocula_80col_forced ||
                          (ext_enabled && ((vid->vid_mode & 0x05) == 0x01));
        bool want_exthires = !vid->ocula_80col_forced && ext_enabled &&
                             ((vid->vid_mode & 0x05) == 0x05);
        /* ULA-NG modes étendus (§5.8) : latchés au début de trame comme les
         * modes OCULA (résolution stable une trame entière). Live via les
         * caches ula_ng ; NULL en test unitaire. */
        bool want_ng_chunky = vid->ng_chunky_active && *vid->ng_chunky_active;
        bool want_ng_text80 = vid->ng_text80_active && *vid->ng_text80_active;
        if (want_80col != vid->ocula_80col ||
            want_exthires != vid->ocula_exthires ||
            want_ng_chunky != vid->ng_chunky ||
            want_ng_text80 != vid->ng_text80) {
            vid->ocula_80col = want_80col;
            vid->ocula_exthires = want_exthires;
            vid->ng_chunky = want_ng_chunky;
            vid->ng_text80 = want_ng_text80;
            apply_profile_resolution(vid);
            memset(vid->framebuffer, 0, sizeof(vid->framebuffer));
            vid->need_refresh = true;
        }
    }

    /* OCULA redefinable palette: re-read at the START OF EVERY SCANLINE
     * (sprint 46). A program that rewrites $BFE0-$BFE7 between scanlines
     * therefore gets per-line palette changes — a single change is the
     * top/bottom split of the 1985 Multicoloric card (Micr'Oric n°9),
     * one change per line gives copper-style rasters, and continuous
     * changes give plasmas. The scanline-accurate main loop samples each
     * line at its real CPU cycle, so the palette seen is the one in RAM
     * at that moment. Inert while locked / on a stock ULA. */
    palette_latch(vid, memory);

    /* Border register ($BFEA) re-read on the same per-scanline schedule:
     * a value rewritten between scanlines paints border raster bars. */
    border_latch(vid, memory, y);

    /* ULA resets attributes at start of every scanline. */
    vid->text_attr = 0;

    if (vid->ocula_80col) {
        render_80col_scanline(vid, memory, y);
        return;
    }
    if (vid->ocula_exthires && y < 200) {
        render_exthires_scanline(vid, memory, y);
        return;
    }
    /* ext-HIRES lines 200-223 fall through to the standard bottom text
     * rows ($BB80): serial attributes still decode there, which is the
     * in-band escape hatch out of the bitmap-only extended mode. */

    /* ULA-NG modes étendus (§5.8) : chunky 4bpp (320) / texte 80 col (480).
     * Modes bitmap/texte plein écran (0-223) — pas de pied de texte 40 col
     * (évite le trou noir 240-319 de la bande de statut hérité du HIRES).
     * Sprites composés en fin de rendu comme les autres modes. */
    if (vid->ng_text80) {
        render_ng_text80_scanline(vid, memory, y);
        if (vid->ng_dev)
            ula_ng_composite_scanline(vid->ng_dev, vid->framebuffer,
                                      vid->native_w, vid->native_h, y);
        if (y == 223) vid->need_refresh = false;
        return;
    }
    if (vid->ng_chunky) {
        render_ng_chunky_scanline(vid, memory, y);   /* plein écran 0-223 */
        if (vid->ng_dev)
            ula_ng_composite_scanline(vid->ng_dev, vid->framebuffer,
                                      vid->native_w, vid->native_h, y);
        if (y == 223) vid->need_refresh = false;
        return;
    }

    if (y < 200) {
        uint8_t ink = ORIC_WHITE, paper = ORIC_BLACK;
        /* ULA-NG scroll fin (§5.5) : décalage pixel X (0-5) / Y (0-7) à la
         * composition, quand actif. Inactif → 0 (compat, rendu identique). */
        int sx = 0, sy = 0;
        if (vid->ng_active && *vid->ng_active) {
            if (vid->ng_scrollx) sx = *vid->ng_scrollx;
            if (vid->ng_scrolly) sy = *vid->ng_scrolly;
        }
        int src_y = y + sy;              /* fine Y : décale la ligne source (contenu vers le haut) */
        int row = src_y / 8;
        int chline = src_y & 7;
        int last_col = sx > 0 ? 40 : 39; /* cellule en plus pour combler le bord droit */

        for (int col = 0; col <= last_col; col++) {
            bool hires = (vid->vid_mode & 0x04) != 0;
            /* ULA-NG start-address (§5.3) : remplace la base du fetch ($A000
             * HIRES / $BB80 TEXT) par NG_SCRSTART quand actif (double buffer /
             * scroll vertical grossier). 0 ou inactif = base par défaut (compat). */
            uint16_t scr_base = hires ? 0xA000 : 0xBB80;
            if (vid->ng_active && *vid->ng_active && vid->ng_scrstart && *vid->ng_scrstart)
                scr_base = *vid->ng_scrstart;
            uint16_t base = hires ? (uint16_t)(scr_base + src_y * 40)
                                  : (uint16_t)(scr_base + row * 40);
            uint8_t byte = memory[base + col];
            int px = col * 6 - sx;       /* fine X : décale l'affichage (set_pixel clippe) */

            /* ULA-NG attributs parallèles (§5.6) : encre+papier par cellule
             * depuis le plan NG, indépendamment du flux pixel (pas de color
             * clash sériel). Actif → tous les octets sont du contenu. */
            bool ng_attr_on = vid->ng_attr_active && *vid->ng_attr_active && vid->ng_attr;
            if (ng_attr_on) {
                uint8_t a = vid->ng_attr[(y * 40 + col) & (ORIC_NG_ATTR_MASK)];
                ink = a & 0x07; paper = (uint8_t)((a >> 3) & 0x07);
            }

            if (!ng_attr_on && (byte & 0x60) == 0) {
                bool inverse = false;
                decode_attr(vid, byte, &ink, &paper, &inverse);
                render_attr_block(vid, px, y, paper, 1, (byte & 0x80) != 0);
            } else if (hires) {
                render_hires_block(vid, px, y, byte, ink, paper);
            } else {
                bool char_inv = (byte & 0x80) != 0;
                if (blink_phase_on(vid)) char_inv = !char_inv;
                /* Inverse complements ink/paper (XOR 7), it does not swap. */
                uint8_t fg = char_inv ? (uint8_t)(ink ^ 0x07) : ink;
                uint8_t bg = char_inv ? (uint8_t)(paper ^ 0x07) : paper;
                uint8_t ir, ig, ib, pr, pg, pb;
                get_rgb(vid, fg, &ir, &ig, &ib);
                get_rgb(vid, bg, &pr, &pg, &pb);
                int erow = effective_chline(vid, chline, row);
                uint8_t bits = get_charset_byte(vid, memory, byte & 0x7F, erow);
                for (int bx = 5; bx >= 0; bx--) {
                    bool on = (bits & (1 << bx)) != 0;
                    if (on) set_pixel(vid, px + (5 - bx), y, ir, ig, ib);
                    else    set_pixel(vid, px + (5 - bx), y, pr, pg, pb);
                }
            }
        }

        if (y == 199) vid->hires_mode = (vid->vid_mode & 0x04) != 0;
    } else {
        render_bottom_text_scanline(vid, memory, y);   /* rangées 200-223 ($BB80) */
        if (y == 223) vid->need_refresh = false;
    }

    /* ULA-NG sprites (§5.7) : composition sur le fond de cette scanline (après
     * le fond, avant présentation) — no-op si inactif. Le module ula_ng possède
     * l'état sprite, la palette et la collision. */
    if (vid->ng_dev)
        ula_ng_composite_scanline(vid->ng_dev, vid->framebuffer,
                                  vid->native_w, vid->native_h, y);
}

void video_render_frame(video_t* vid, const uint8_t* memory) {
    if (!memory) return;
    /* Fallback path: render the whole frame in one pass against the current
     * memory snapshot. Used when the main loop hasn't ticked per-scanline
     * (e.g. headless --screenshot at exit) or as a last resort. */
    for (int y = 0; y < 224; y++) video_render_scanline(vid, memory, y);
}
