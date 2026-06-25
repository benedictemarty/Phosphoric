/**
 * @file test_ocula.c
 * @brief OCULA ULA-profile unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-12
 * @version 1.17.0-alpha
 *
 * Sprint 39 — pluggable ULA profile infrastructure. The OCULA profile
 * (RP2350-based ULA replacement, forum.defence-force.org t=2709) must be
 * selectable, persistent across reset, and — at this stage — render
 * pixel-identical to the stock HCS 10017 profile.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video/video.h"
#include "video/export.h"
#include "io/ocula_io.h"
#include "io/ocula_gpu.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected 0x%llX, got 0x%llX\n", __FILE__, __LINE__, \
               (unsigned long long)(b), (unsigned long long)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 1: Default profile is the stock HCS 10017                 */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_default_profile_is_hcs10017) {
    video_t vid;
    video_init(&vid);
    ASSERT_EQ(video_get_profile(&vid), ULA_PROFILE_HCS10017);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 2: Profile set/get roundtrip                              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_set_get_profile) {
    video_t vid;
    video_init(&vid);
    video_set_profile(&vid, ULA_PROFILE_OCULA);
    ASSERT_EQ(video_get_profile(&vid), ULA_PROFILE_OCULA);
    video_set_profile(&vid, ULA_PROFILE_HCS10017);
    ASSERT_EQ(video_get_profile(&vid), ULA_PROFILE_HCS10017);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 3: Profile survives video_reset (chip in the socket)      */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_profile_survives_reset) {
    video_t vid;
    video_init(&vid);
    video_set_profile(&vid, ULA_PROFILE_OCULA);
    video_reset(&vid);
    ASSERT_EQ(video_get_profile(&vid), ULA_PROFILE_OCULA);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 4: Profile names                                          */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_profile_names) {
    ASSERT_TRUE(strcmp(video_profile_name(ULA_PROFILE_HCS10017), "ula") == 0);
    ASSERT_TRUE(strcmp(video_profile_name(ULA_PROFILE_OCULA), "ocula") == 0);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 5: Profile parsing (CLI --ula argument)                   */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_profile_parse_valid) {
    ASSERT_EQ(video_profile_parse("ula"), ULA_PROFILE_HCS10017);
    ASSERT_EQ(video_profile_parse("ULA"), ULA_PROFILE_HCS10017);
    ASSERT_EQ(video_profile_parse("hcs10017"), ULA_PROFILE_HCS10017);
    ASSERT_EQ(video_profile_parse("ocula"), ULA_PROFILE_OCULA);
    ASSERT_EQ(video_profile_parse("OCULA"), ULA_PROFILE_OCULA);
}

TEST(test_profile_parse_invalid) {
    ASSERT_EQ(video_profile_parse(NULL), -1);
    ASSERT_EQ(video_profile_parse(""), -1);
    ASSERT_EQ(video_profile_parse("hcs"), -1);
    ASSERT_EQ(video_profile_parse("oculaa"), -1);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 6: Native resolution (240x224 for both profiles today)    */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_native_resolution_hcs10017) {
    video_t vid;
    video_init(&vid);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
    ASSERT_EQ(vid.native_h, ORIC_SCREEN_H);
}

TEST(test_native_resolution_ocula) {
    video_t vid;
    video_init(&vid);
    video_set_profile(&vid, ULA_PROFILE_OCULA);
    /* No extended mode active yet: OCULA boots in stock resolution */
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
    ASSERT_EQ(vid.native_h, ORIC_SCREEN_H);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 7: Framebuffer capacity covers OCULA maximum modes        */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_framebuffer_capacity) {
    video_t vid;
    ASSERT_EQ(OCULA_MAX_W, 480);
    ASSERT_EQ(OCULA_MAX_H, 448);
    ASSERT_TRUE(sizeof(vid.framebuffer) == (size_t)(OCULA_MAX_W * OCULA_MAX_H * 3));
    (void)vid;
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 8: Switching profile clears framebuffer + flags refresh   */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_profile_switch_clears_framebuffer) {
    video_t vid;
    video_init(&vid);
    vid.framebuffer[0] = 0xAA;
    vid.need_refresh = false;
    video_set_profile(&vid, ULA_PROFILE_OCULA);
    ASSERT_EQ(vid.framebuffer[0], 0x00);
    ASSERT_TRUE(vid.need_refresh);
}

TEST(test_profile_switch_same_is_noop) {
    video_t vid;
    video_init(&vid);
    vid.framebuffer[0] = 0xAA;
    vid.need_refresh = false;
    video_set_profile(&vid, ULA_PROFILE_HCS10017);
    /* Same profile: framebuffer untouched, no refresh forced */
    ASSERT_EQ(vid.framebuffer[0], 0xAA);
    ASSERT_FALSE(vid.need_refresh);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 9: OCULA renders pixel-identical to HCS 10017 (Étape 1)   */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_ocula_renders_identical_to_hcs10017) {
    static uint8_t memory[0x10000];
    static video_t vid_ula, vid_ocula;
    memset(memory, 0, sizeof(memory));

    /* Fake charset for 'A' (0x41) at TEXT charset base $B400 */
    for (int row = 0; row < 8; row++)
        memory[0xB400 + 0x41 * 8 + row] = (uint8_t)(0x15 << (row & 1));
    /* Text screen: attribute INK red, then a row of 'A' */
    memory[0xBB80] = 0x01;
    for (int col = 1; col < 40; col++)
        memory[0xBB80 + col] = 0x41;

    video_init(&vid_ula);
    video_render_frame(&vid_ula, memory);

    video_init(&vid_ocula);
    video_set_profile(&vid_ocula, ULA_PROFILE_OCULA);
    video_render_frame(&vid_ocula, memory);

    ASSERT_EQ(vid_ula.native_w, vid_ocula.native_w);
    ASSERT_EQ(vid_ula.native_h, vid_ocula.native_h);
    size_t active = (size_t)(vid_ula.native_w * vid_ula.native_h * 3);
    ASSERT_TRUE(memcmp(vid_ula.framebuffer, vid_ocula.framebuffer, active) == 0);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  80-COLUMN MODE (Sprint 40 — extended serial attribute 25)      */
/* ═══════════════════════════════════════════════════════════════ */

static uint8_t mem80[0x10000];

/* Helper: fresh OCULA video + clean memory; glyph 'A' (0x41) row 0
 * = all 6 pixels set in the standard TEXT charset at $B400. */
static void setup_80col(video_t* vid) {
    memset(mem80, 0, sizeof(mem80));
    for (int row = 0; row < 8; row++) {
        mem80[0xB400 + 0x41 * 8 + row] = 0x3F;  /* std charset: solid */
        mem80[0xB800 + 0x41 * 8 + row] = 0x2A;  /* alt charset: distinct */
    }
    video_init(vid);
    video_set_profile(vid, ULA_PROFILE_OCULA);
    vid->ocula_unlocked = true;  /* sprint 45: arm the opt-in extensions */
}

static uint8_t pixel_r(const video_t* vid, int x, int y) {
    return vid->framebuffer[(y * vid->native_w + x) * 3];
}
static uint8_t pixel_g(const video_t* vid, int x, int y) {
    return vid->framebuffer[(y * vid->native_w + x) * 3 + 1];
}

TEST(test_80col_latch_via_attr25) {
    static video_t vid;
    setup_80col(&vid);
    mem80[0xBB80] = 25;  /* extended serial attribute: 80-column text */
    video_render_frame(&vid, mem80);   /* attr decoded during this frame */
    video_render_frame(&vid, mem80);   /* latch fires at next frame start */
    ASSERT_TRUE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, OCULA_MAX_W);
    ASSERT_EQ(vid.native_h, ORIC_SCREEN_H);
}

TEST(test_80col_ignored_on_stock_ula) {
    static video_t vid;
    setup_80col(&vid);
    video_set_profile(&vid, ULA_PROFILE_HCS10017);
    mem80[0xBB80] = 25;
    video_render_frame(&vid, mem80);
    video_render_frame(&vid, mem80);
    /* Stock HCS 10017: bit 0 is a don't-care, stays 40-column TEXT */
    ASSERT_FALSE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
}

TEST(test_80col_default_boot_is_40col) {
    static video_t vid;
    setup_80col(&vid);
    video_render_frame(&vid, mem80);  /* powerup vid_mode = 2: bit 0 clear */
    ASSERT_FALSE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
}

TEST(test_80col_fetches_from_a000) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x01;
    mem80[OCULA_80COL_BASE] = 0x41;   /* 'A' at row 0, col 0 */
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_80col);
    /* Solid glyph row, default ink white: pixel (0,0) is white */
    ASSERT_EQ(pixel_r(&vid, 0, 0), 0xFF);
    ASSERT_EQ(pixel_g(&vid, 0, 0), 0xFF);
}

TEST(test_80col_column_79_renders) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x01;
    /* Columns 0-77 hold 0x00 bytes = INK black attributes (faithful ULA
     * behavior), so re-arm INK white at column 78 before the glyph. */
    mem80[OCULA_80COL_BASE + 78] = 0x07;
    mem80[OCULA_80COL_BASE + 79] = 0x41;
    video_render_frame(&vid, mem80);
    /* Column 79 occupies x = 474..479 */
    ASSERT_EQ(pixel_r(&vid, 474, 0), 0xFF);
    ASSERT_EQ(pixel_r(&vid, 479, 0), 0xFF);
    /* Column 78 untouched: black */
    ASSERT_EQ(pixel_r(&vid, 473, 0), 0x00);
}

TEST(test_80col_serial_attributes_per_column) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x01;
    mem80[OCULA_80COL_BASE]     = 0x01;  /* attr: INK red */
    mem80[OCULA_80COL_BASE + 1] = 0x41;  /* 'A' rendered in red */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 6, 0), 0xFF);  /* col 1, red channel on */
    ASSERT_EQ(pixel_g(&vid, 6, 0), 0x00);  /* green channel off */
}

TEST(test_80col_alt_charset_attr) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x01;
    mem80[OCULA_80COL_BASE]     = 0x09;  /* attr: alternate charset */
    mem80[OCULA_80COL_BASE + 1] = 0x41;  /* 'A' from $B800: 0x2A pattern */
    video_render_frame(&vid, mem80);
    /* 0x2A = 0b101010: bit 5 set -> x offset 0 lit, bit 4 clear -> x 1 dark */
    ASSERT_EQ(pixel_r(&vid, 6, 0), 0xFF);
    ASSERT_EQ(pixel_r(&vid, 7, 0), 0x00);
}

TEST(test_80col_bottom_row_27) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x01;
    mem80[OCULA_80COL_BASE + 27 * OCULA_80COL_COLS] = 0x41;
    video_render_frame(&vid, mem80);
    /* Row 27 chline 7 = scanline 223 */
    ASSERT_EQ(pixel_r(&vid, 0, 223), 0xFF);
}

TEST(test_80col_back_to_40col_via_attr26) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x01;
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_80col);
    mem80[OCULA_80COL_BASE] = 26;  /* attr: TEXT 50Hz, bit 0 clear */
    video_render_frame(&vid, mem80);   /* attr decoded from 80-col stream */
    video_render_frame(&vid, mem80);   /* latch drops at frame start */
    ASSERT_FALSE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
}

TEST(test_80col_latch_via_attr27_50hz) {
    static video_t vid;
    setup_80col(&vid);
    /* Attr 27 = bit 0 (80-col) + bit 1 (50 Hz): frequency bit stays
     * independent, 80-col activates — canonical PAL activation. */
    vid.vid_mode = 0x03;
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, OCULA_MAX_W);
}

TEST(test_80col_hires_bit_wins) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;  /* bit 0 set BUT HIRES bit set: not 80-col */
    video_render_frame(&vid, mem80);
    ASSERT_FALSE(vid.ocula_80col);
    /* Since étape 3, attr 29 (bit 0 + HIRES) selects extended HIRES */
    ASSERT_TRUE(vid.ocula_exthires);
    ASSERT_EQ(vid.native_w, OCULA_EXTHIRES_W);
}

TEST(test_80col_ppm_export_dimensions) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x01;
    video_render_frame(&vid, mem80);
    const char* path = "/tmp/oric1_test_80col.ppm";
    ASSERT_TRUE(video_export_ppm(&vid, path));
    FILE* fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    char magic[3] = {0};
    int w = 0, h = 0;
    int n = fscanf(fp, "%2s %d %d", magic, &w, &h);
    fclose(fp);
    remove(path);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(w, 480);
    ASSERT_EQ(h, 224);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  EXTENDED HIRES 320x200 + PALETTE (Sprint 41 — attrs 29/31)     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_exthires_latch_via_attr29) {
    static video_t vid;
    setup_80col(&vid);
    mem80[0xBB80] = 29;  /* extended serial attribute: ext-HIRES */
    video_render_frame(&vid, mem80);
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_exthires);
    ASSERT_FALSE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, OCULA_EXTHIRES_W);
    ASSERT_EQ(vid.native_h, ORIC_SCREEN_H);
}

TEST(test_exthires_attr31_50hz_variant) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x07;  /* attr 31: bits 0+1+2 — frequency independent */
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_exthires);
}

TEST(test_exthires_ignored_on_stock_ula) {
    static video_t vid;
    setup_80col(&vid);
    video_set_profile(&vid, ULA_PROFILE_HCS10017);
    vid.vid_mode = 0x05;
    video_render_frame(&vid, mem80);
    /* Stock ULA: attr 29 is plain HIRES, 240 px */
    ASSERT_FALSE(vid.ocula_exthires);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
}

TEST(test_exthires_8_pixels_per_byte) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    mem80[OCULA_EXTHIRES_BASE] = 0x80;      /* MSB = leftmost pixel */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 0, 0), 0xFF);   /* bit 7 lit */
    ASSERT_EQ(pixel_r(&vid, 1, 0), 0x00);   /* bit 6 dark */
}

TEST(test_exthires_no_attribute_decoding) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    /* 0x07 would be an INK attribute in stock HIRES; here it is pure
     * bitmap: 3 rightmost pixels of the byte lit. */
    mem80[OCULA_EXTHIRES_BASE] = 0x07;
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 4, 0), 0x00);
    ASSERT_EQ(pixel_r(&vid, 5, 0), 0xFF);
    ASSERT_EQ(pixel_r(&vid, 7, 0), 0xFF);
    /* And the mode did not change (no attr decode in bitmap area) */
    ASSERT_TRUE(vid.ocula_exthires);
}

TEST(test_exthires_column_39_renders_to_319) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    mem80[OCULA_EXTHIRES_BASE + 39] = 0x01;  /* LSB = rightmost pixel */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 319, 0), 0xFF);
    ASSERT_EQ(pixel_r(&vid, 318, 0), 0x00);
}

TEST(test_exthires_bottom_rows_are_text) {
    static video_t vid;
    setup_80col(&vid);
    /* Hires charsets at $9800 (ext-HIRES keeps standard bottom rows) */
    for (int row = 0; row < 8; row++)
        mem80[0x9800 + 0x41 * 8 + row] = 0x3F;
    vid.vid_mode = 0x05;
    mem80[0xBB80 + 25 * 40] = 0x41;  /* 'A' in text row 25 (y=200) */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 0, 200), 0xFF);   /* glyph rendered */
    ASSERT_EQ(pixel_r(&vid, 250, 200), 0x00); /* x 240-319 stays blank */
}

TEST(test_exthires_escape_via_bottom_text_attr) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_exthires);
    /* Bottom text rows still decode attributes: in-band escape hatch */
    mem80[0xBB80 + 25 * 40] = 26;  /* attr TEXT 50 Hz */
    video_render_frame(&vid, mem80);
    video_render_frame(&vid, mem80);
    ASSERT_FALSE(vid.ocula_exthires);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
}

TEST(test_palette_redefine_with_magic) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    mem80[OCULA_EXTHIRES_BASE] = 0x80;
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_PAL_BASE + 7]  = 0xE0;  /* ink (entry 7) -> pure red RGB332 */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 0, 0), 0xFF);
    ASSERT_EQ(pixel_g(&vid, 0, 0), 0x00);
}

TEST(test_palette_ignored_without_magic) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    mem80[OCULA_EXTHIRES_BASE] = 0x80;
    mem80[OCULA_PAL_BASE + 7]  = 0xE0;  /* no magic: standard palette */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 0, 0), 0xFF);
    ASSERT_EQ(pixel_g(&vid, 0, 0), 0xFF);  /* still white */
}

TEST(test_palette_ignored_on_stock_ula) {
    static video_t vid;
    setup_80col(&vid);
    video_set_profile(&vid, ULA_PROFILE_HCS10017);
    mem80[0xBB80] = 0x41;
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_PAL_BASE + 7]  = 0xE0;
    video_render_frame(&vid, mem80);
    /* Stock ULA never reads the palette block: glyph stays white */
    ASSERT_EQ(pixel_g(&vid, 0, 0), 0xFF);
}

TEST(test_palette_applies_in_text_mode) {
    static video_t vid;
    setup_80col(&vid);
    /* 40-col text under OCULA, ink red redefined to pure green */
    mem80[0xBB80]     = 0x01;  /* attr INK red */
    mem80[0xBB80 + 1] = 0x41;  /* 'A' */
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_PAL_BASE + 1]  = 0x1C;  /* entry 1 -> pure green RGB332 */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 6, 0), 0x00);
    ASSERT_EQ(pixel_g(&vid, 6, 0), 0xFF);
}

TEST(test_palette_restores_when_magic_removed) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    mem80[OCULA_EXTHIRES_BASE] = 0x80;
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_PAL_BASE + 7]  = 0xE0;
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_g(&vid, 0, 0), 0x00);  /* red ink active */
    mem80[OCULA_PAL_MAGIC] = 0x00;          /* disarm */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_g(&vid, 0, 0), 0xFF);  /* back to white */
}

/* ───────────────────────── Border register ($BFEA) ───────────────────── */

/* Armed + unlocked: $BFEA decodes as RGB332 into the per-scanline border. */
TEST(test_border_armed_decodes_rgb332) {
    static video_t vid;
    setup_80col(&vid);                  /* OCULA + unlocked */
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_BORDER_REG]    = 0x03;  /* RGB332 000 000 11 = pure blue */
    video_render_scanline(&vid, mem80, 0);
    uint8_t r, g, b;
    video_get_border_rgb(&vid, 0, &r, &g, &b);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0xFF);
}

/* No magic: the block is plain storage, border stays black (stock Oric). */
TEST(test_border_inert_without_magic) {
    static video_t vid;
    setup_80col(&vid);
    mem80[OCULA_BORDER_REG] = 0xE0;     /* would be red if interpreted */
    video_render_scanline(&vid, mem80, 0);
    uint8_t r, g, b;
    video_get_border_rgb(&vid, 0, &r, &g, &b);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00);
}

/* Stock HCS 10017 never scans the block: border stays black. */
TEST(test_border_inert_on_stock_ula) {
    static video_t vid;
    setup_80col(&vid);
    video_set_profile(&vid, ULA_PROFILE_HCS10017);
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_BORDER_REG]    = 0xE0;
    video_render_scanline(&vid, mem80, 0);
    uint8_t r, g, b;
    video_get_border_rgb(&vid, 0, &r, &g, &b);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00);
}

/* OCULA but still locked: opt-in gate keeps the border inert. */
TEST(test_border_inert_until_unlock) {
    static video_t vid;
    setup_80col(&vid);
    vid.ocula_unlocked = false;         /* re-lock */
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_BORDER_REG]    = 0xE0;
    video_render_scanline(&vid, mem80, 0);
    uint8_t r, g, b;
    video_get_border_rgb(&vid, 0, &r, &g, &b);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00);
}

/* Armed with $BFEA=$00: black border = identical to a stock Oric. */
TEST(test_border_zero_is_black_when_armed) {
    static video_t vid;
    setup_80col(&vid);
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_BORDER_REG]    = 0x00;
    video_render_scanline(&vid, mem80, 0);
    uint8_t r, g, b;
    video_get_border_rgb(&vid, 0, &r, &g, &b);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00);
}

/* Per-scanline re-read: rewriting $BFEA between lines = border raster bars. */
TEST(test_border_raster_per_scanline) {
    static video_t vid;
    setup_80col(&vid);
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    /* even lines -> red (0xE0), odd lines -> green (0x1C) */
    for (int y = 0; y < 4; y++) {
        mem80[OCULA_BORDER_REG] = (y & 1) ? 0x1C : 0xE0;
        video_render_scanline(&vid, mem80, y);
    }
    uint8_t r, g, b;
    video_get_border_rgb(&vid, 0, &r, &g, &b);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0x00);   /* line 0: red */
    video_get_border_rgb(&vid, 1, &r, &g, &b);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0xFF);   /* line 1: green */
    video_get_border_rgb(&vid, 2, &r, &g, &b);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0x00);   /* line 2: red again */
}

/* ──────────────────── Border overscan compositing (Sprint 65) ─────────── */

static uint8_t composed[OCULA_BORDERED_MAX_W * OCULA_BORDERED_MAX_H * 3];

/* Read a composited-output pixel given the total width tw. */
static uint8_t cpx(int tw, int x, int y, int c) {
    return composed[((size_t)y * tw + x) * 3 + c];
}

/* Composite dimensions = active + 2× the per-side border. */
TEST(test_border_compose_dimensions) {
    static video_t vid;
    setup_80col(&vid);
    video_render_frame(&vid, mem80);     /* 40-col text: native 240x224 */
    int w = 0, h = 0;
    video_compose_bordered(&vid, composed, &w, &h);
    ASSERT_EQ(w, ORIC_SCREEN_W + 2 * OCULA_BORDER_W);
    ASSERT_EQ(h, ORIC_SCREEN_H + 2 * OCULA_BORDER_H);
    ASSERT_EQ(video_bordered_w(&vid), w);
    ASSERT_EQ(video_bordered_h(&vid), h);
}

/* The active image lands inset by (OCULA_BORDER_W, OCULA_BORDER_H). */
TEST(test_border_compose_active_centered) {
    static video_t vid;
    setup_80col(&vid);
    /* Redefine ink so an active glyph pixel is a known non-black colour. */
    mem80[0xBB80]     = 0x01;             /* INK red */
    mem80[0xBB80 + 1] = 0x41;             /* 'A' */
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_PAL_BASE + 1]  = 0x1C;    /* entry 1 -> pure green */
    video_render_frame(&vid, mem80);
    int w = 0, h = 0;
    video_compose_bordered(&vid, composed, &w, &h);
    /* active (6,0) is the first glyph pixel -> composite (6+BW, BH). */
    ASSERT_EQ(cpx(w, 6 + OCULA_BORDER_W, OCULA_BORDER_H, 0), pixel_r(&vid, 6, 0));
    ASSERT_EQ(cpx(w, 6 + OCULA_BORDER_W, OCULA_BORDER_H, 1), pixel_g(&vid, 6, 0));
}

/* Armed border colour fills the left/right and top/bottom bands. */
TEST(test_border_compose_bands_colored) {
    static video_t vid;
    setup_80col(&vid);
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_BORDER_REG]    = 0xE0;    /* pure red, all lines */
    video_render_frame(&vid, mem80);
    int w = 0, h = 0;
    video_compose_bordered(&vid, composed, &w, &h);
    ASSERT_EQ(cpx(w, 0, OCULA_BORDER_H, 0), 0xFF);      /* left band, row 0 */
    ASSERT_EQ(cpx(w, 0, OCULA_BORDER_H, 1), 0x00);
    ASSERT_EQ(cpx(w, w - 1, OCULA_BORDER_H, 0), 0xFF);  /* right band, row 0 */
    ASSERT_EQ(cpx(w, 0, 0, 0), 0xFF);                  /* top band (reuses line 0) */
    ASSERT_EQ(cpx(w, 0, h - 1, 0), 0xFF);              /* bottom band (last line) */
}

/* Disarmed / stock: every band is black. */
TEST(test_border_compose_black_when_disarmed) {
    static video_t vid;
    setup_80col(&vid);
    mem80[OCULA_BORDER_REG] = 0xE0;       /* no magic -> ignored */
    video_render_frame(&vid, mem80);
    int w = 0, h = 0;
    video_compose_bordered(&vid, composed, &w, &h);
    ASSERT_EQ(cpx(w, 0, 0, 0), 0x00);
    ASSERT_EQ(cpx(w, 0, OCULA_BORDER_H, 0), 0x00);
    ASSERT_EQ(cpx(w, w - 1, h - 1, 0), 0x00);
}

/* Per-scanline border = different band colour per output row. */
TEST(test_border_compose_raster_bands) {
    static video_t vid;
    setup_80col(&vid);
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    for (int y = 0; y < 4; y++) {
        mem80[OCULA_BORDER_REG] = (y & 1) ? 0x1C : 0xE0;  /* green / red */
        video_render_scanline(&vid, mem80, y);
    }
    int w = 0, h = 0;
    video_compose_bordered(&vid, composed, &w, &h);
    ASSERT_EQ(cpx(w, 0, OCULA_BORDER_H + 0, 0), 0xFF);  /* line 0 left band red */
    ASSERT_EQ(cpx(w, 0, OCULA_BORDER_H + 0, 1), 0x00);
    ASSERT_EQ(cpx(w, 0, OCULA_BORDER_H + 1, 0), 0x00);  /* line 1 left band green */
    ASSERT_EQ(cpx(w, 0, OCULA_BORDER_H + 1, 1), 0xFF);
}

TEST(test_exthires_ppm_export_dimensions) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    video_render_frame(&vid, mem80);
    const char* path = "/tmp/oric1_test_exthires.ppm";
    ASSERT_TRUE(video_export_ppm(&vid, path));
    FILE* fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    char magic[3] = {0};
    int w = 0, h = 0;
    int n = fscanf(fp, "%2s %d %d", magic, &w, &h);
    fclose(fp);
    remove(path);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(w, 320);
    ASSERT_EQ(h, 224);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  ID REGISTERS + MEMORY BANKING (Sprint 42 — $03E0-$03E7)        */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_id_registers) {
    memory_t mem;
    memory_init(&mem);
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_ID0), 'O');
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_ID1), 'C');
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_CAPS), OCULA_CAPS_ALL);
    memory_cleanup(&mem);
}

TEST(test_id_registers_read_only) {
    memory_t mem;
    memory_init(&mem);
    ocula_io_write(&mem, OCULA_IO_ID0, 0xAA);
    ocula_io_write(&mem, OCULA_IO_CAPS, 0x00);
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_ID0), 'O');
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_CAPS), OCULA_CAPS_ALL);
    memory_cleanup(&mem);
}

TEST(test_reserved_regs_read_zero) {
    memory_t mem;
    memory_init(&mem);
    for (uint16_t a = 0x03E4; a <= OCULA_IO_END; a++)
        ASSERT_EQ(ocula_io_read(&mem, a), 0x00);
    memory_cleanup(&mem);
}

TEST(test_bank_default_zero) {
    memory_t mem;
    memory_init(&mem);
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_BANK), 0);
    ASSERT_TRUE(mem.ocula_bank_mem == NULL);  /* no alloc until used */
    memory_cleanup(&mem);
}

TEST(test_bank_switch_isolates_window) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, 0xA000, 11);            /* bank 0 */
    ocula_io_write(&mem, OCULA_IO_BANK, 1);
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_BANK), 1);
    ASSERT_EQ(memory_read(&mem, 0xA000), 0);   /* bank 1 starts clean */
    memory_write(&mem, 0xA000, 22);
    ASSERT_EQ(memory_read(&mem, 0xA000), 22);
    ocula_io_write(&mem, OCULA_IO_BANK, 0);
    ASSERT_EQ(memory_read(&mem, 0xA000), 11);  /* bank 0 untouched */
    ocula_io_write(&mem, OCULA_IO_BANK, 1);
    ASSERT_EQ(memory_read(&mem, 0xA000), 22);  /* bank 1 persisted */
    memory_cleanup(&mem);
}

TEST(test_bank_value_masked_to_3_bits) {
    memory_t mem;
    memory_init(&mem);
    ocula_io_write(&mem, OCULA_IO_BANK, 0x0A);  /* 10 & 7 = 2 */
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_BANK), 2);
    memory_cleanup(&mem);
}

TEST(test_bank_window_bounds) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, 0x9FFF, 33);            /* below window */
    memory_write(&mem, 0xBFFF, 44);            /* last window byte, bank 0 */
    ocula_io_write(&mem, OCULA_IO_BANK, 3);
    memory_write(&mem, 0xBFFF, 55);
    ASSERT_EQ(memory_read(&mem, 0x9FFF), 33);  /* outside: always bank 0 */
    ASSERT_EQ(memory_read(&mem, 0xBFFF), 55);
    ocula_io_write(&mem, OCULA_IO_BANK, 0);
    ASSERT_EQ(memory_read(&mem, 0xBFFF), 44);
    memory_cleanup(&mem);
}

TEST(test_ula_always_scans_bank_0) {
    static video_t vid;
    memory_t mem;
    memory_init(&mem);
    memset(mem.ram, 0, RAM_SIZE);
    for (int row = 0; row < 8; row++)
        mem.ram[0xB400 + 0x41 * 8 + row] = 0x3F;
    mem.ram[0xBB80] = 0x41;                    /* 'A' in bank 0 screen */
    video_init(&vid);
    video_set_profile(&vid, ULA_PROFILE_OCULA);
    ocula_io_write(&mem, OCULA_IO_BANK, 2);    /* CPU on side bank */
    /* The ULA renders from mem.ram (bank 0) regardless of CPU bank */
    video_render_frame(&vid, mem.ram);
    ASSERT_EQ(vid.framebuffer[0], 0xFF);
    memory_cleanup(&mem);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  OCULA-GPU (Sprint 43 — $03E8-$03EF, étape 5)                   */
/* ═══════════════════════════════════════════════════════════════ */

/* Helper: memory + video + gpu, arg block at $0400. Writes the 16-byte
 * argument block then fires the opcode. */
static void gpu_setup(memory_t* mem, video_t* vid, ocula_gpu_t* gpu) {
    memory_init(mem);
    memset(mem->ram, 0, RAM_SIZE);
    video_init(vid);
    video_set_profile(vid, ULA_PROFILE_OCULA);
    ocula_gpu_init(gpu);
    vid->ocula_unlocked = true;  /* sprint 45: GPU users have opted in */
    ocula_gpu_write(gpu, mem, vid, OCULA_GPU_PTRL, 0x00);
    ocula_gpu_write(gpu, mem, vid, OCULA_GPU_PTRH, 0x04);
}

static void gpu_args(memory_t* mem, const uint8_t* a, int n) {
    for (int i = 0; i < n; i++) mem->ram[0x0400 + i] = a[i];
}

TEST(test_gpu_caps_bit) {
    memory_t mem;
    memory_init(&mem);
    ASSERT_EQ(ocula_io_read(&mem, OCULA_IO_CAPS), 0x1F);
    ASSERT_TRUE(OCULA_CAPS_ALL & OCULA_CAP_GPU);
    memory_cleanup(&mem);
}

TEST(test_gpu_ptr_readback) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    ASSERT_EQ(ocula_gpu_read(&gpu, OCULA_GPU_PTRL), 0x00);
    ASSERT_EQ(ocula_gpu_read(&gpu, OCULA_GPU_PTRH), 0x04);
    ASSERT_EQ(ocula_gpu_read(&gpu, OCULA_GPU_STATUS), OCULA_GPU_ST_READY);
    memory_cleanup(&mem);
}

TEST(test_gpu_info) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_INFO);
    ASSERT_EQ(gpu.status, OCULA_GPU_ST_READY);
    ASSERT_EQ(mem.ram[0x0400], OCULA_GPU_INFO_VERSION);
    ASSERT_EQ(mem.ram[0x0401], OCULA_GPU_INFO_SPRITES);
    ASSERT_EQ(mem.ram[0x0402], OCULA_GPU_INFO_OPMASK);
    memory_cleanup(&mem);
}

TEST(test_gpu_fill) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    /* FILL dst=$A000 stride=40 w=10 h=10 val=$FF */
    uint8_t args[] = {0x00, 0xA0, 40, 10, 10, 0xFF};
    gpu_args(&mem, args, 6);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_FILL);
    ASSERT_EQ(gpu.status, OCULA_GPU_ST_READY);
    ASSERT_EQ(mem.ram[0xA000], 0xFF);            /* coin haut-gauche */
    ASSERT_EQ(mem.ram[0xA000 + 9 * 40 + 9], 0xFF); /* coin bas-droit */
    ASSERT_EQ(mem.ram[0xA000 + 10], 0x00);       /* hors largeur */
    ASSERT_EQ(mem.ram[0xA000 + 10 * 40], 0x00);  /* hors hauteur */
    memory_cleanup(&mem);
}

TEST(test_gpu_fill_respects_active_bank) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    memory_ocula_set_bank(&mem, 2);
    uint8_t args[] = {0x00, 0xA0, 1, 1, 1, 0x77};
    gpu_args(&mem, args, 6);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_FILL);
    ASSERT_EQ(memory_read(&mem, 0xA000), 0x77);  /* banque 2 */
    memory_ocula_set_bank(&mem, 0);
    ASSERT_EQ(memory_read(&mem, 0xA000), 0x00);  /* banque 0 intacte */
    memory_cleanup(&mem);
}

TEST(test_gpu_copy) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    mem.ram[0x5000] = 0xAA;
    mem.ram[0x5001] = 0xBB;
    mem.ram[0x5028] = 0xCC;  /* ligne 2 (stride 40) */
    /* COPY src=$5000 sstr=40 dst=$A000 dstr=40 w=2 h=2 */
    uint8_t args[] = {0x00, 0x50, 40, 0x00, 0xA0, 40, 2, 2};
    gpu_args(&mem, args, 8);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_COPY);
    ASSERT_EQ(gpu.status, OCULA_GPU_ST_READY);
    ASSERT_EQ(mem.ram[0xA000], 0xAA);
    ASSERT_EQ(mem.ram[0xA001], 0xBB);
    ASSERT_EQ(mem.ram[0xA028], 0xCC);
    memory_cleanup(&mem);
}

TEST(test_gpu_copy_overlap_safe) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    for (int i = 0; i < 8; i++) mem.ram[0x5000 + i] = (uint8_t)(i + 1);
    /* Copie recouvrante vers l'avant : dst = src+2, 1 ligne de 8 */
    uint8_t args[] = {0x00, 0x50, 8, 0x02, 0x50, 8, 8, 1};
    gpu_args(&mem, args, 8);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_COPY);
    /* Snapshot complet avant écriture : pas de propagation en cascade */
    ASSERT_EQ(mem.ram[0x5002], 1);
    ASSERT_EQ(mem.ram[0x5009], 8);
    memory_cleanup(&mem);
}

TEST(test_gpu_scroll_registers_and_render) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    vid.vid_mode = 0x05;  /* HIRES étendu */
    mem.ram[0xA000] = 0x80;  /* pixel à (0,0) */
    uint8_t args[] = {8, 1};  /* dx=8 px, dy=1 ligne */
    gpu_args(&mem, args, 2);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_SCROLL);
    ASSERT_EQ(vid.ocula_scroll_x, 8);
    ASSERT_EQ(vid.ocula_scroll_y, 1);
    video_render_frame(&vid, mem.ram);
    /* Source (0,0) apparaît à l'écran en x=(0-8) mod 320=312, y=199 :
     * fetch écran (312,199) ← source ((312+8)%320=0, (199+1)%200=0) ✓ */
    ASSERT_EQ(pixel_r(&vid, 312, 199), 0xFF);
    ASSERT_EQ(pixel_r(&vid, 0, 0), 0x00);
    memory_cleanup(&mem);
}

TEST(test_gpu_scroll_zero_is_off) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    vid.vid_mode = 0x05;
    mem.ram[0xA000] = 0x80;
    uint8_t args[] = {0, 0};
    gpu_args(&mem, args, 2);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_SCROLL);
    video_render_frame(&vid, mem.ram);
    ASSERT_EQ(pixel_r(&vid, 0, 0), 0xFF);
    memory_cleanup(&mem);
}

TEST(test_gpu_wait_vbl_requires_blocking) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_WAIT_VBL);
    ASSERT_EQ(gpu.status, OCULA_GPU_ERR_NOBLOCK);
    ASSERT_FALSE(gpu.wait_vbl);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD,
                    OCULA_GPU_OP_WAIT_VBL | 0x80);
    ASSERT_EQ(gpu.status, OCULA_GPU_ST_BUSY);
    ASSERT_TRUE(gpu.wait_vbl);
    memory_cleanup(&mem);
}

TEST(test_gpu_bad_opcode) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, 0x42);
    ASSERT_EQ(gpu.status, OCULA_GPU_ERR_BADOP);
    memory_cleanup(&mem);
}

TEST(test_gpu_bad_arg_ptr) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_PTRL, 0x00);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_PTRH, 0x02);  /* $0200 < $0400 */
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_INFO);
    ASSERT_EQ(gpu.status, OCULA_GPU_ERR_BADADDR);
    memory_cleanup(&mem);
}

TEST(test_gpu_fill_protects_low_memory) {
    memory_t mem; static video_t vid; ocula_gpu_t gpu;
    gpu_setup(&mem, &vid, &gpu);
    /* FILL dst=$0200 → rejeté (zéro page/pile/système/I-O protégés) */
    uint8_t args[] = {0x00, 0x02, 1, 8, 1, 0xFF};
    gpu_args(&mem, args, 6);
    ocula_gpu_write(&gpu, &mem, &vid, OCULA_GPU_CMD, OCULA_GPU_OP_FILL);
    ASSERT_EQ(gpu.status, OCULA_GPU_ERR_BADADDR);
    ASSERT_EQ(mem.ram[0x0200], 0x00);
    memory_cleanup(&mem);
}

/* ─────────────────────────────────────────────────────────────── */
/*  Sprint 44 — OCULA 80-col BASIC mirror                          */
/* ─────────────────────────────────────────────────────────────── */

TEST(test_80col_mirror_basic_write) {
    /* A write to $BB80 (first char, 40-col screen) must be mirrored
     * to $A000 row 0, col 0 when ocula_80col_mirror is active. */
    memory_t mem;
    memory_init(&mem);
    mem.ocula_80col_mirror = true;
    memory_write(&mem, 0xBB80, 0x41); /* 'A' */
    ASSERT_EQ(mem.ram[0xBB80], 0x41);
    ASSERT_EQ(mem.ram[0xA000], 0x41); /* mirrored at row 0 col 0 */
    memory_cleanup(&mem);
}

TEST(test_80col_mirror_row_col_mapping) {
    /* Write to $BB80 + 80 = $BBD0 → row 2, col 0 of 40-col (off=80, row=2, col=0)
     * → 80-col addr = $A000 + 2*80 + 0 = $A0A0 */
    memory_t mem;
    memory_init(&mem);
    mem.ocula_80col_mirror = true;
    uint16_t src = 0xBB80 + 80; /* row 2 col 0 */
    memory_write(&mem, src, 0x42);
    ASSERT_EQ(mem.ram[src], 0x42);
    ASSERT_EQ(mem.ram[0xA000 + 2 * 80 + 0], 0x42);
    memory_cleanup(&mem);
}

TEST(test_80col_mirror_disabled_by_default) {
    /* Without the flag the 40-col write must NOT touch $A000. */
    memory_t mem;
    memory_init(&mem);
    mem.ram[0xA000] = 0x00;
    memory_write(&mem, 0xBB80, 0x41);
    ASSERT_EQ(mem.ram[0xBB80], 0x41);
    ASSERT_EQ(mem.ram[0xA000], 0x00);
    memory_cleanup(&mem);
}

TEST(test_80col_forced_latch_survives_vid_mode) {
    /* ocula_80col_forced must keep ocula_80col=true even when
     * vid_mode has no attr 25/27 set (standard BASIC mode). */
    static video_t vid;
    video_init(&vid);
    video_set_profile(&vid, ULA_PROFILE_OCULA);
    vid.ocula_80col_forced = true;
    uint8_t mem[65536];
    memset(mem, 0, sizeof(mem));
    /* vid_mode=2 (default, TEXT, no bit 0) — forced must override */
    vid.vid_mode = 0x02;
    video_render_scanline(&vid, mem, 0); /* evaluates latches */
    ASSERT_TRUE(vid.ocula_80col);
}

/* ─────────────────────────────────────────────────────────────── */
/*  Sprint 45 — opt-in unlock (blind-write ROM), Dbug t=2709 review  */
/* ─────────────────────────────────────────────────────────────── */

/* An OCULA-equipped Oric must render byte-for-byte like a stock machine
 * until software opts in. attr 25 (80-col) is inert while locked. */
TEST(test_optin_80col_inert_until_unlock) {
    static video_t vid;
    setup_80col(&vid);
    vid.ocula_unlocked = false;        /* locked: stock behaviour */
    vid.vid_mode = 0x01;               /* attr 25 */
    video_render_frame(&vid, mem80);
    video_render_frame(&vid, mem80);
    ASSERT_FALSE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
    /* Now opt in: the very same attr starts driving 80-col */
    vid.ocula_unlocked = true;
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_80col);
    ASSERT_EQ(vid.native_w, OCULA_MAX_W);
}

TEST(test_optin_exthires_inert_until_unlock) {
    static video_t vid;
    setup_80col(&vid);
    vid.ocula_unlocked = false;
    vid.vid_mode = 0x05;               /* attr 29 */
    video_render_frame(&vid, mem80);
    ASSERT_FALSE(vid.ocula_exthires);
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
    vid.ocula_unlocked = true;
    video_render_frame(&vid, mem80);
    ASSERT_TRUE(vid.ocula_exthires);
    ASSERT_EQ(vid.native_w, OCULA_EXTHIRES_W);
}

/* $BFE0-$BFFF stays plain storage while locked: the magic bytes are not
 * interpreted, so a game keeping data there (Encounter, Symoon) is safe. */
TEST(test_optin_palette_inert_until_unlock) {
    static video_t vid;
    setup_80col(&vid);
    vid.ocula_unlocked = false;
    mem80[0xBB80]     = 0x01;          /* attr INK red */
    mem80[0xBB80 + 1] = 0x41;          /* 'A' */
    mem80[OCULA_PAL_MAGIC]     = 'O';  /* looks like the magic... */
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    mem80[OCULA_PAL_BASE + 1]  = 0x1C; /* ...but stays plain data */
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 6, 0), 0xFF);   /* standard red ink */
    ASSERT_EQ(pixel_g(&vid, 6, 0), 0x00);
    /* Opt in: now the palette redefinition takes effect */
    vid.ocula_unlocked = true;
    video_render_frame(&vid, mem80);
    ASSERT_EQ(pixel_r(&vid, 6, 0), 0x00);
    ASSERT_EQ(pixel_g(&vid, 6, 0), 0xFF);   /* redefined to pure green */
}

/* Blind-write ROM knock: 'O' then 'C' to the unlock page arms the chip. */
TEST(test_unlock_knock_sequence) {
    memory_t mem;
    memory_init(&mem);
    ASSERT_FALSE(memory_ocula_unlocked(&mem));
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_O);
    ASSERT_FALSE(memory_ocula_unlocked(&mem));   /* mid-knock */
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_C);
    ASSERT_TRUE(memory_ocula_unlocked(&mem));
    memory_cleanup(&mem);
}

/* Any address in the $FB page works (ULA decodes A8-A15 only). */
TEST(test_unlock_knock_decodes_on_page) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, OCULA_UNLOCK_PAGE + 0x37, OCULA_UNLOCK_O);
    memory_write(&mem, OCULA_UNLOCK_PAGE + 0xC2, OCULA_UNLOCK_C);
    ASSERT_TRUE(memory_ocula_unlocked(&mem));
    memory_cleanup(&mem);
}

/* 'C' without a preceding 'O' must not unlock; an interrupted knock resets. */
TEST(test_unlock_requires_ordered_knock) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_C);   /* lone 'C' */
    ASSERT_FALSE(memory_ocula_unlocked(&mem));
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_O);
    memory_write(&mem, OCULA_UNLOCK_PAGE, 0x55);            /* breaks knock */
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_C);
    ASSERT_FALSE(memory_ocula_unlocked(&mem));
    memory_cleanup(&mem);
}

/* Writing the lock command re-locks the chip. */
TEST(test_unlock_relock) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_O);
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_C);
    ASSERT_TRUE(memory_ocula_unlocked(&mem));
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_LOCK);
    ASSERT_FALSE(memory_ocula_unlocked(&mem));
    memory_cleanup(&mem);
}

/* A RAM-overlay write to the same address is real RAM, not an unlock:
 * only genuine ROM blind-writes arm the chip. */
TEST(test_unlock_ignored_in_ram_overlay) {
    memory_t mem;
    memory_init(&mem);
    mem.basic_rom_disabled = true;   /* $C000-$FFFF is RAM overlay */
    mem.overlay_active = false;
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_O);
    memory_write(&mem, OCULA_UNLOCK_PAGE, OCULA_UNLOCK_C);
    ASSERT_FALSE(memory_ocula_unlocked(&mem));
    /* The bytes landed in RAM as a normal write */
    ASSERT_EQ(mem.upper_ram[OCULA_UNLOCK_PAGE - 0xC000], OCULA_UNLOCK_C);
    memory_cleanup(&mem);
}

/* ─────────────────────────────────────────────────────────────── */
/*  Sprint 46 — palette per-scanline (rasters / plasma, Multicoloric) */
/* ─────────────────────────────────────────────────────────────── */

/* The redefinable palette is re-read at every scanline, so rewriting an
 * entry between scanlines changes the colour of the lines below — within
 * the SAME frame. This generalises the Multicoloric card (single fixed
 * top/bottom split) to an arbitrary number of mid-frame changes.
 *
 * Plasma: alternate palette entry 7 (ext-HIRES ink) red/green per line,
 * rendering scanline by scanline, and check each line took the colour
 * armed just before it was drawn. With a per-frame latch every line
 * would share line 0's colour — this test would fail. */
TEST(test_palette_plasma_per_scanline) {
    static video_t vid;
    setup_80col(&vid);                  /* OCULA + unlocked */
    vid.vid_mode = 0x05;                /* ext-HIRES: ink = palette entry 7 */
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    /* Leftmost pixel lit on every bitmap row (MSB) */
    for (int y = 0; y < 200; y++)
        mem80[OCULA_EXTHIRES_BASE + y * 40] = 0x80;
    /* Render line by line, re-arming entry 7 before each scanline:
     * even lines -> pure red (0xE0), odd lines -> pure green (0x1C). */
    for (int y = 0; y < 200; y++) {
        mem80[OCULA_PAL_BASE + 7] = (y & 1) ? 0x1C : 0xE0;
        video_render_scanline(&vid, mem80, y);
    }
    ASSERT_TRUE(vid.ocula_exthires);
    /* Same frame, three consecutive lines, three palette states */
    ASSERT_EQ(pixel_r(&vid, 0, 0), 0xFF);  /* line 0: red */
    ASSERT_EQ(pixel_g(&vid, 0, 0), 0x00);
    ASSERT_EQ(pixel_r(&vid, 0, 1), 0x00);  /* line 1: green */
    ASSERT_EQ(pixel_g(&vid, 0, 1), 0xFF);
    ASSERT_EQ(pixel_r(&vid, 0, 2), 0xFF);  /* line 2: red again */
    ASSERT_EQ(pixel_g(&vid, 0, 2), 0x00);
}

/* Two-zone split (the literal Multicoloric use case): one palette for the
 * top of the screen, another for the bottom, switched at a chosen line. */
TEST(test_palette_split_two_zones) {
    static video_t vid;
    setup_80col(&vid);
    vid.vid_mode = 0x05;
    mem80[OCULA_PAL_MAGIC]     = 'O';
    mem80[OCULA_PAL_MAGIC + 1] = 'C';
    for (int y = 0; y < 200; y++)
        mem80[OCULA_EXTHIRES_BASE + y * 40] = 0x80;
    for (int y = 0; y < 200; y++) {
        mem80[OCULA_PAL_BASE + 7] = (y < 100) ? 0xE0 : 0x1C;  /* red / green */
        video_render_scanline(&vid, mem80, y);
    }
    ASSERT_EQ(pixel_r(&vid, 0, 99),  0xFF);  /* top zone: red */
    ASSERT_EQ(pixel_g(&vid, 0, 99),  0x00);
    ASSERT_EQ(pixel_r(&vid, 0, 100), 0x00);  /* bottom zone: green */
    ASSERT_EQ(pixel_g(&vid, 0, 100), 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST RUNNER                                                     */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  OCULA ULA Profile Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_default_profile_is_hcs10017);
    RUN(test_set_get_profile);
    RUN(test_profile_survives_reset);
    RUN(test_profile_names);
    RUN(test_profile_parse_valid);
    RUN(test_profile_parse_invalid);
    RUN(test_native_resolution_hcs10017);
    RUN(test_native_resolution_ocula);
    RUN(test_framebuffer_capacity);
    RUN(test_profile_switch_clears_framebuffer);
    RUN(test_profile_switch_same_is_noop);
    RUN(test_ocula_renders_identical_to_hcs10017);
    RUN(test_80col_latch_via_attr25);
    RUN(test_80col_ignored_on_stock_ula);
    RUN(test_80col_default_boot_is_40col);
    RUN(test_80col_fetches_from_a000);
    RUN(test_80col_column_79_renders);
    RUN(test_80col_serial_attributes_per_column);
    RUN(test_80col_alt_charset_attr);
    RUN(test_80col_bottom_row_27);
    RUN(test_80col_back_to_40col_via_attr26);
    RUN(test_80col_latch_via_attr27_50hz);
    RUN(test_80col_hires_bit_wins);
    RUN(test_80col_ppm_export_dimensions);
    RUN(test_exthires_latch_via_attr29);
    RUN(test_exthires_attr31_50hz_variant);
    RUN(test_exthires_ignored_on_stock_ula);
    RUN(test_exthires_8_pixels_per_byte);
    RUN(test_exthires_no_attribute_decoding);
    RUN(test_exthires_column_39_renders_to_319);
    RUN(test_exthires_bottom_rows_are_text);
    RUN(test_exthires_escape_via_bottom_text_attr);
    RUN(test_palette_redefine_with_magic);
    RUN(test_palette_ignored_without_magic);
    RUN(test_palette_ignored_on_stock_ula);
    RUN(test_palette_applies_in_text_mode);
    RUN(test_palette_restores_when_magic_removed);
    RUN(test_border_armed_decodes_rgb332);
    RUN(test_border_inert_without_magic);
    RUN(test_border_inert_on_stock_ula);
    RUN(test_border_inert_until_unlock);
    RUN(test_border_zero_is_black_when_armed);
    RUN(test_border_raster_per_scanline);
    RUN(test_border_compose_dimensions);
    RUN(test_border_compose_active_centered);
    RUN(test_border_compose_bands_colored);
    RUN(test_border_compose_black_when_disarmed);
    RUN(test_border_compose_raster_bands);
    RUN(test_exthires_ppm_export_dimensions);
    RUN(test_id_registers);
    RUN(test_id_registers_read_only);
    RUN(test_reserved_regs_read_zero);
    RUN(test_bank_default_zero);
    RUN(test_bank_switch_isolates_window);
    RUN(test_bank_value_masked_to_3_bits);
    RUN(test_bank_window_bounds);
    RUN(test_ula_always_scans_bank_0);
    RUN(test_gpu_caps_bit);
    RUN(test_gpu_ptr_readback);
    RUN(test_gpu_info);
    RUN(test_gpu_fill);
    RUN(test_gpu_fill_respects_active_bank);
    RUN(test_gpu_copy);
    RUN(test_gpu_copy_overlap_safe);
    RUN(test_gpu_scroll_registers_and_render);
    RUN(test_gpu_scroll_zero_is_off);
    RUN(test_gpu_wait_vbl_requires_blocking);
    RUN(test_gpu_bad_opcode);
    RUN(test_gpu_bad_arg_ptr);
    RUN(test_gpu_fill_protects_low_memory);
    RUN(test_80col_mirror_basic_write);
    RUN(test_80col_mirror_row_col_mapping);
    RUN(test_80col_mirror_disabled_by_default);
    RUN(test_80col_forced_latch_survives_vid_mode);

    RUN(test_optin_80col_inert_until_unlock);
    RUN(test_optin_exthires_inert_until_unlock);
    RUN(test_optin_palette_inert_until_unlock);
    RUN(test_unlock_knock_sequence);
    RUN(test_unlock_knock_decodes_on_page);
    RUN(test_unlock_requires_ordered_knock);
    RUN(test_unlock_relock);
    RUN(test_unlock_ignored_in_ram_overlay);

    RUN(test_palette_plasma_per_scanline);
    RUN(test_palette_split_two_zones);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
