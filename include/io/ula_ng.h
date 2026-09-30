/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file ula_ng.h
 * @brief ULA-NG — "next-gen" ULA for the Oric (software reference for a future
 *        Verilog port on Tang Primer 20K). Step 1: lock/identity +
 *        page 3 plumbing. See docs/ula-ng/AUDIT.md and ULA-NG-SPEC.md.
 * @author bmarty <bmarty@mailo.com>
 *
 * Module isolated at "FPGA mirror" boundaries: only 3 interfaces
 * (page 3 registers, scanline tick — later steps —, IRQ line).
 *
 * Register window: $0340-$035F (free in the community memory map).
 * At reset, ULA-NG is indistinguishable from an HCS10017: locked, the window
 * goes to the VIA (reads) and writes land there too (the module merely
 * watches $0340 for the unlock sequence) → bit-for-bit non-regression.
 * After the sequence 'N','G' ($4E,$47) on $0340, the module arms the extensions
 * and owns the window.
 */

#ifndef ULA_NG_H
#define ULA_NG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>   /* FILE*: savestate serialization (ula_ng_save/load) */

#define ULA_NG_WINDOW_LO   0x0340u
#define ULA_NG_WINDOW_HI   0x035Fu
#define ULA_NG_REG_LOCK    0x0340u   /* NG_LOCK (W) / NG_ID (R) */
#define ULA_NG_REG_MODE    0x0341u   /* NG_MODE: b0 = extensions active */
#define ULA_NG_REG_SCR_LO  0x0342u   /* NG_SCRSTART lo: video fetch base (LSB) */
#define ULA_NG_REG_SCR_HI  0x0343u   /* NG_SCRSTART hi: video fetch base (MSB) */
#define ULA_NG_REG_SCROLLX 0x0344u   /* NG_SCROLLX: fine X offset (0-5 pixels) */
#define ULA_NG_REG_SCROLLY 0x0345u   /* NG_SCROLLY: fine Y offset (0-7 pixels) */
#define ULA_NG_REG_RASTER  0x0346u   /* NG_RASTERLINE: line triggering the IRQ */
#define ULA_NG_REG_STATUS  0x0347u   /* NG_STATUS: R b7=raster IRQ pending;
                                        W = acknowledge + b0 = raster IRQ enable */
#define ULA_NG_REG_PAL_IDX 0x0348u   /* NG_PAL_IDX: LUT index (0-15), auto-incr */
#define ULA_NG_REG_PAL_LO  0x0349u   /* NG_PAL_DATA lo: 0000RRRR */
#define ULA_NG_REG_PAL_HI  0x034Au   /* NG_PAL_DATA hi: GGGGBBBB (commit + incr) */
#define ULA_NG_REG_COP_CTRL 0x034Bu  /* NG_COP_CTRL: write = reset copper list */
#define ULA_NG_REG_COP_DATA 0x034Cu  /* NG_COP_DATA: stream of 3 bytes/entry (§5.4):
                                        [0]=line [1]=(index<<4)|R [2]=(G<<4)|B */
#define ULA_NG_REG_ATTR_FILL 0x034Du /* NG_ATTR_FILL: fills the whole plane (§5.6) */
#define ULA_NG_REG_ATTR_DATA 0x034Eu /* NG_ATTR_DATA: stream of 1 byte/cell (auto-incr) */
#define ULA_NG_REG_IDCHK   0x034Fu   /* NG_IDCHK (R) = ~NG_ID (handshake) */
#define ULA_NG_REG_SPR_CTRL 0x0350u  /* NG_SPR_CTRL: b0 = global sprite enable (§5.7) */
#define ULA_NG_REG_SPR_SEL  0x0351u  /* NG_SPR_SEL: selected sprite 0-15 + pattern ptr reset */
#define ULA_NG_REG_SPR_X    0x0352u  /* NG_SPR_X: X position (0-255) of the selected sprite */
#define ULA_NG_REG_SPR_Y    0x0353u  /* NG_SPR_Y: Y position (0-255) */
#define ULA_NG_REG_SPR_ATTR 0x0354u  /* NG_SPR_ATTR: b0 = sprite visible */
#define ULA_NG_REG_SPR_DATA 0x0355u  /* NG_SPR_DATA: pattern stream 1 byte/px (0=transparent, 1-7=index) */
#define ULA_NG_REG_SPR_STATUS 0x0356u/* NG_SPR_STATUS (R): b7 = collision (clear on read) */
#define ULA_NG_REG_VDU     0x0357u   /* NG_VDU (W): built-in VDU command stream (docs/ula-ng/VDU.md) */
#define ULA_NG_VDU_MAXPARAMS 4       /* max params of a VDU command */
/* Chunky VRAM held by the ULA-NG (VDU v0.2): the VDU owns its pixels.
 * 160×224 pixels 4bpp, 2 px/byte → 80 bytes/row. */
#define ULA_NG_VRAM_W      160
#define ULA_NG_VRAM_H      224
#define ULA_NG_VRAM_STRIDE 80
#define ULA_NG_VRAM_SIZE   (ULA_NG_VRAM_STRIDE * ULA_NG_VRAM_H)  /* 17920 */
#define ULA_NG_COP_MAX     64        /* max entries in the copper list */
#define ULA_NG_MODE_ATTR   0x02u     /* NG_MODE b1: parallel attributes active */
#define ULA_NG_MODE_VIDMASK 0x0Cu    /* NG_MODE b2-3: video mode (§5.8) */
#define ULA_NG_VIDMODE_CHUNKY 0x04u  /* b2-3 = 01: chunky 4bpp 160×200 */
#define ULA_NG_VIDMODE_TEXT80 0x08u  /* b2-3 = 10: 80-column text */
#define ULA_NG_ATTR_SIZE   8192      /* attribute plane: 8 KB (ink+paper/cell) */
#define ULA_NG_STATUS_IRQ  0x80u     /* NG_STATUS b7 (R): raster IRQ pending */
#define ULA_NG_STATUS_EN   0x01u     /* NG_STATUS b0 (W): raster IRQ enable */
#define ULA_NG_FRAME_LINES 312       /* lines of a full PAL frame (0-311) */
#define ULA_NG_VERSION     0x1Eu     /* NG_ID when unlocked (v1.0) */
#define ULA_NG_UNLOCK_N    0x4Eu     /* 'N' */
#define ULA_NG_UNLOCK_G    0x47u     /* 'G' */
#define ULA_NG_MODE_ENABLE 0x01u     /* NG_MODE b0 */
#define ULA_NG_PAL_ENTRIES 16        /* LUT of 16 entries × 12 bits (RGB444) */
#define ULA_NG_SPRITES     16        /* number of hardware sprites (§5.7) */
#define ULA_NG_SPR_DIM     16        /* 16×16 sprites */
#define ULA_NG_SPR_PIXELS  (ULA_NG_SPR_DIM * ULA_NG_SPR_DIM) /* 256 px/sprite */
#define ULA_NG_SPR_MAXW    512       /* max framebuffer width (collision occupancy) */
#define ULA_NG_SPR_STATUS_COL 0x80u  /* NG_SPR_STATUS b7: collision detected */

typedef struct ula_ng_s {
    bool    unlocked;      /* extensions armed (after the sequence) */
    uint8_t unlock_step;   /* 0 = idle, 1 = 'N' seen, waiting for 'G' */
    uint8_t regs[0x20];    /* register file $0340-$035F */

    /* Palette indirection (§5.1): 16-entry LUT, stored already expanded to
     * RGB888 (RGB444→888 expansion by nibble replication on write).
     * At reset, the first 8 entries = Oric colors (identity → compatibility). */
    uint8_t pal[ULA_NG_PAL_ENTRIES][3];
    uint8_t pal_idx;       /* current NG_PAL_IDX (0-15) */
    uint8_t pal_r;         /* latched R nibble, waiting for G/B */
    bool    active;    /* = unlocked && NG_MODE.b0 (general NG gate: video hooks) */

    /* Start address (§5.3): replaces the video fetch base ($A000/$BB80).
     * 0 = use the mode's default base (compatibility). */
    uint16_t scrstart;

    /* Fine scroll (§5.5): pixel-precise offset at composition time. */
    uint8_t scrollx;       /* NG_SCROLLX: 0-5 (cell width = 6 px) */
    uint8_t scrolly;       /* NG_SCROLLY: 0-7 (cell height = 8 px) */

    /* Raster IRQ (§5.2) */
    uint8_t raster_line;   /* NG_RASTERLINE: line (frame 0-311) triggering the IRQ */
    bool    raster_enable; /* NG_STATUS.b0: raster IRQ armed */
    bool    raster_pending;/* raster IRQ pending (b7), until acknowledged */

    /* Per-scanline palette (§5.4): mini-copper. List of entries (line, LUT
     * index, RGB888 color) applied by ula_ng_scanline during hblank. */
    struct { uint8_t line, index, r, g, b; } copper[ULA_NG_COP_MAX];
    uint8_t copper_count;   /* number of programmed entries */
    uint8_t copper_phase;   /* 0/1/2: current byte of the streamed entry */
    uint8_t cop_line, cop_index, cop_r;  /* partial entry being assembled */

    /* Parallel attributes (§5.6): ink+paper plane per cell, outside the
     * 6502's 64 KB (FPGA DDR3 mirror). Active if NG_MODE.b1. Indexed
     * (scanline*40 + col); byte = (paper<<3)|ink. */
    uint8_t attr[ULA_NG_ATTR_SIZE];
    uint16_t attr_wp;       /* write pointer (auto-increment) */
    bool    attr_active;    /* = unlocked && NG_MODE.b1 (cache for the video hook) */

    /* Hardware sprites (§5.7): up to 16 sprites of 16×16, palette-indexed
     * pattern (0 = transparent, 1-7 = LUT index). Table outside the 6502's
     * 64 KB (FPGA DDR3 mirror), programmed by streaming through the register
     * window. Composited in the output pipeline (after the background);
     * priority by index (0 in front); sprite-sprite collision detection. */
    struct {
        uint8_t x, y;                        /* screen position (px) */
        bool    enable;                      /* NG_SPR_ATTR.b0: visible */
        uint8_t pattern[ULA_NG_SPR_PIXELS];  /* 0=transparent, 1-7=palette index */
    } sprites[ULA_NG_SPRITES];
    uint8_t  spr_sel;       /* sprite selected for programming */
    uint16_t spr_wp;        /* pattern write pointer (0-255, auto-increment) */
    bool     spr_enable;    /* NG_SPR_CTRL.b0: global enable */
    bool     spr_collision; /* sprite-sprite collision (status b7), clear on read */
    bool     spr_active;    /* = unlocked && spr_enable (cache for the video hook) */

    /* Extended video modes (§5.8), selected by NG_MODE.b2-3 (caches for the
     * start-of-frame video latch). chunky 4bpp = 160×200 16 colors (NG
     * LUT); 80-column text = redefinable RAM charset. Data read from
     * NG_SCRSTART (default $A000). */
    bool     chunky_active; /* = active && NG_MODE.b2-3 == 01 */
    bool     text80_active; /* = active && NG_MODE.b2-3 == 10 */

    /* Built-in VDU (docs/ula-ng/VDU.md): NG_VDU command port ($0357). The
     * 6502 streams bytes; the interpreter (below = stand-in for the FPGA
     * soft-core firmware) translates them into existing register writes.
     * Allocation-free FSM: current code + collected parameters. */
    uint8_t  vdu_cmd;                          /* command code in progress */
    uint8_t  vdu_params[ULA_NG_VDU_MAXPARAMS]; /* collected parameters */
    uint8_t  vdu_need;                         /* number of expected parameters (0 = waiting for a code) */
    uint8_t  vdu_got;                          /* number of parameters already collected */

    /* Chunky VRAM held by the ULA-NG (VDU v0.2). When vram_active, the chunky
     * mode reads these pixels instead of CPU RAM (NG_SCRSTART). Fed by the
     * graphics VDU commands (CLG/PLOT/DRAW), current color vdu_gcol. */
    uint8_t  vram[ULA_NG_VRAM_SIZE];
    bool     vram_active;                      /* chunky reads the ULA-NG VRAM */
    uint8_t  vdu_gcol;                          /* current drawing color (0-15) */

    /* VDU upload protocol (v0.3, Agon-style "buffered commands"): after
     * VDU 23 (begin sprite pattern), the next vdu_upload bytes are
     * streamed into the selected sprite's pattern (spr_sel/spr_wp reused). */
    uint16_t vdu_upload;                        /* bytes left to stream (0 = inactive) */
} ula_ng_t;

/** Initializes (= reset: locked HCS10017 state, registers at 0). */
void ula_ng_init(ula_ng_t* u);

/** Hardware reset: re-locks everything and clears the registers to 0. */
void ula_ng_reset(ula_ng_t* u);

/** Savestate hook — writes the ULA-NG state to `fp`. Returns **false when there
 *  is nothing to save** (locked = default state) so that NO section is emitted
 *  (byte-identical .ost in regular use). Serialized as a blob (the state is a
 *  pointer-free POD) → *same-build* savestate: see ula_ng_load. */
bool ula_ng_save(const ula_ng_t* u, FILE* fp);

/** Savestate hook — reads the ULA-NG state back. **Size guard**: if `size` does
 *  not match sizeof(ula_ng_t) (other build/arch/version), the state is NOT
 *  overwritten (stays at default) rather than corrupted. */
void ula_ng_load(ula_ng_t* u, FILE* fp, uint32_t size);

/** True if the address is within the $0340-$035F register window. */
static inline int ula_ng_addr_in_window(uint16_t addr) {
    return addr >= ULA_NG_WINDOW_LO && addr <= ULA_NG_WINDOW_HI;
}

/** True if the module intercepts the window (unlocked). Otherwise reads
 *  must fall back to the VIA (indistinguishable). */
bool ula_ng_active(const ula_ng_t* u);

/** Reads a register (called only when ula_ng_active()). */
uint8_t ula_ng_read(ula_ng_t* u, uint16_t addr);

/** Writes a register. Returns 1 if CONSUMED (do not fall back to the VIA),
 *  0 otherwise (locked → VIA passthrough + silent watching of the lock). */
int ula_ng_write(ula_ng_t* u, uint16_t addr, uint8_t value);

/** Scanline tick (full frame line 0-311). Raises the raster IRQ (pending)
 *  when line == NG_RASTERLINE, if armed (unlocked && NG_MODE.b0 && enable).
 *  To be called by the video loop for each line. */
void ula_ng_scanline(ula_ng_t* u, int line);

/** State of the IRQ line to the 6502 (true = raster IRQ active). */
bool ula_ng_irq(const ula_ng_t* u);

/** Composites the sprites (§5.7) onto scanline `y` of the RGB888 framebuffer
 *  (width `w`, height `h`). Transparent = index 0; color = NG palette LUT.
 *  Priority by index (sprite 0 in front). Sets `spr_collision` on sprite-sprite
 *  overlap. No-op if `!spr_active`. To be called after rendering the background. */
void ula_ng_composite_scanline(ula_ng_t* u, uint8_t* fb, int w, int h, int y);

#endif /* ULA_NG_H */
