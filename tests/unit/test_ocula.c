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
    ASSERT_EQ(vid.native_w, ORIC_SCREEN_W);
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

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
