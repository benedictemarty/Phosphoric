/**
 * @file video.h
 * @brief ORIC-1 video system interface
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>
#include <stdbool.h>

struct ula_ng_s;   /* io/ula_ng.h — sprites §5.7 (couplage évité) */

#define ORIC_SCREEN_W   240
#define ORIC_SCREEN_H   224
/* Maximum framebuffer dimensions across all video modes. Room for the
 * ULA-NG extended modes : 80-column text (480 px, VIDEO_MAX_W) and doubled
 * vertical. The framebuffer is allocated at this capacity ; the active
 * resolution is native_w x native_h (240x224 en standard). */
#define VIDEO_MAX_W     480
#define VIDEO_MAX_H     448
#define ORIC_TEXT_COLS   40
#define ORIC_TEXT_ROWS   28
#define ORIC_HIRES_W    240
#define ORIC_HIRES_H    200
#define ORIC_CHAR_W     6
#define ORIC_CHAR_H     8
#define ORIC_FPS         50

/* Largeur du mode « large » (chunky ULA-NG §5.8 : 160 px × 2 = 320). */
#define VIDEO_WIDE_W     320

/* Visible overscan band composited around the active image at presentation
 * time (Sprint 65). The active framebuffer keeps its native_w x native_h
 * dimensions and coordinate system unchanged — the border only exists in the
 * composited output (SDL window / optional export). Each active scanline y
 * paints its own video_border[y] colour into that line's left/right bands;
 * the top/bottom bands reuse the first/last active line's colour. */
#define VIDEO_BORDER_W 32   /* left/right overscan, framebuffer px per side */
#define VIDEO_BORDER_H 24   /* top/bottom overscan, framebuffer px per side */
#define VIDEO_BORDERED_MAX_W (VIDEO_MAX_W + 2 * VIDEO_BORDER_W)
#define VIDEO_BORDERED_MAX_H (VIDEO_MAX_H + 2 * VIDEO_BORDER_H)

/* ORIC colors */
#define ORIC_BLACK   0
#define ORIC_RED     1
#define ORIC_GREEN   2
#define ORIC_YELLOW  3
#define ORIC_BLUE    4
#define ORIC_MAGENTA 5
#define ORIC_CYAN    6
#define ORIC_WHITE   7

typedef struct video_s {
    uint8_t framebuffer[VIDEO_MAX_W * VIDEO_MAX_H * 3]; /* RGB888, native_w x native_h used */
    int native_w;           /* Active framebuffer width (stride) in pixels */
    int native_h;           /* Active framebuffer height in pixels */
    bool hires_mode;
    bool need_refresh;
    uint8_t* screen_ram;    /* Pointer into memory $BB80 (text) or $A000 (hires) */
    uint8_t* charset;       /* Character set ROM */
    uint8_t vid_mode;       /* ULA video mode (persistent, set by serial attrs 24-31).
                             * Bit 2: HIRES when set. Initialized to 2 (TEXT/PAL50). */

    /* ULA-NG palette-indirection (§5.1). Wired from emulator_t.ula_ng ; NULL in
     * the bare unit-test path. When active, the NG LUT overrides pal_rgb. */
    const uint8_t (*ng_pal)[3];       /**< -> ula_ng.pal[16][3] (RGB888) */
    const bool*    ng_active;     /**< -> ula_ng.active (gate général NG) */
    const uint16_t* ng_scrstart;  /**< -> ula_ng.scrstart (base fetch, 0=défaut, §5.3) */
    const uint8_t*  ng_scrollx;   /**< -> ula_ng.scrollx (0-5, §5.5) */
    const uint8_t*  ng_scrolly;   /**< -> ula_ng.scrolly (0-7, §5.5) */
    const uint8_t*  ng_attr;      /**< -> ula_ng.attr[8192] (§5.6) */
    const bool*     ng_attr_active; /**< -> ula_ng.attr_active */
#define ORIC_NG_ATTR_MASK 0x1FFFu /* masque du plan d'attributs (8192-1) */

    /* ULA-NG sprites (§5.7) : le module ula_ng possède l'état sprite (table,
     * palette, collision) ; video appelle ula_ng_composite_scanline en fin de
     * scanline via ce pointeur. NULL dans le chemin de test unitaire (pas de
     * sprites). Forward-declaré pour ne pas coupler video.h à io/ula_ng.h. */
    struct ula_ng_s* ng_dev;      /**< -> emulator_t.ula_ng (composition sprites) */

    /* ULA-NG modes vidéo étendus (§5.8). Pointeurs live vers les caches ula_ng
     * (NULL en test unitaire) ; latchés au début de trame dans ng_chunky /
     * ng_text80 (la résolution reste stable une trame entière). */
    const bool* ng_chunky_active; /**< -> ula_ng.chunky_active (chunky 4bpp 320px) */
    const bool* ng_text80_active; /**< -> ula_ng.text80_active (texte 80col 480px) */
    bool        ng_chunky;        /**< latch trame : mode chunky actif */
    bool        ng_text80;        /**< latch trame : mode 80 colonnes actif */
    const uint8_t* ng_vram;       /**< -> ula_ng.vram (VRAM chunky ULA-NG, VDU v0.2) */
    const bool*    ng_vram_active;/**< -> ula_ng.vram_active (chunky lit la VRAM) */

    /* Active palette, RGB888 per Oric color 0-7 (palette standard ; la LUT
     * ULA-NG §5.1 la remplace quand active). */
    uint8_t pal_rgb[8][3];

    /* Couleur de bordure overscan par scanline (RGB888) — infra générique de
     * compositing (video_compose_bordered). Reste noire (aucune source de
     * couleur en standard) ; exposée via video_get_border_rgb(). */
    uint8_t video_border[VIDEO_MAX_H][3];

    /* Serial-attribute group 0x08-0x0F (text attrs).
     * bit 0 = alt charset, bit 1 = double height, bit 2 = blink.
     * Reset to 0 at start of every scanline; latched per-column. */
    uint8_t text_attr;

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

/* Couleur de bordure overscan pour la scanline y (0-223), RGB888 (noire en
 * standard — infra générique de compositing). */
void video_get_border_rgb(const video_t* vid, int y,
                          uint8_t* r, uint8_t* g, uint8_t* b);

/* Total dimensions of the bordered (composited) output for the current mode. */
int  video_bordered_w(const video_t* vid);
int  video_bordered_h(const video_t* vid);

/* Compose the active framebuffer surrounded by the per-scanline overscan border
 * into `out` (RGB888, caller-provided, capacity >= bordered_w*bordered_h*3,
 * which never exceeds VIDEO_BORDERED_MAX_W*VIDEO_BORDERED_MAX_H*3). The total
 * dimensions are returned via *w and *h when non-NULL. */
void video_compose_bordered(const video_t* vid, uint8_t* out, int* w, int* h);

#endif
