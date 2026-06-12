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

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
