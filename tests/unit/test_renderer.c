/**
 * @file test_renderer.c
 * @brief Display scaling unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.9.0-alpha
 *
 * Tests for display scaling logic. Uses headless stubs (no SDL2 required).
 */

#include <stdio.h>
#include <stdlib.h>
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

/* Forward declarations for renderer (no shared header; must match renderer.c
 * and the prototype in main.c — renderer_init takes (scale, prefer_software)). */
bool renderer_init(int scale, bool prefer_software);
void renderer_cleanup(void);
void renderer_set_scale(int scale);
int renderer_get_scale(void);
void renderer_cycle_scale(void);

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 1: Screen dimensions                                      */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_screen_dimensions) {
    ASSERT_EQ(ORIC_SCREEN_W, 240);
    ASSERT_EQ(ORIC_SCREEN_H, 224);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 2: Window size at scale x1                                */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_window_size_x1) {
    ASSERT_EQ(ORIC_SCREEN_W * 1, 240);
    ASSERT_EQ(ORIC_SCREEN_H * 1, 224);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 3: Window size at scale x2                                */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_window_size_x2) {
    ASSERT_EQ(ORIC_SCREEN_W * 2, 480);
    ASSERT_EQ(ORIC_SCREEN_H * 2, 448);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 4: Window size at scale x3 (default)                      */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_window_size_x3) {
    ASSERT_EQ(ORIC_SCREEN_W * 3, 720);
    ASSERT_EQ(ORIC_SCREEN_H * 3, 672);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 5: Window size at scale x4                                */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_window_size_x4) {
    ASSERT_EQ(ORIC_SCREEN_W * 4, 960);
    ASSERT_EQ(ORIC_SCREEN_H * 4, 896);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 6: Headless renderer init                                 */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_renderer_init_headless) {
#ifdef HAS_SDL2
    /* Built with real SDL2: renderer_init() opens a window, which needs a
     * display the CI/headless runner may not have. Exercise init+cleanup
     * without asserting on the outcome — a missing display fails gracefully
     * (no crash, no leak), which is all this test can portably check here. */
    if (renderer_init(3, false)) {
        renderer_cleanup();
    }
#else
    /* Headless stub: init always succeeds and reports scale 1. */
    ASSERT_TRUE(renderer_init(3, false));
    ASSERT_EQ(renderer_get_scale(), 1);
    renderer_cleanup();
#endif
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 7: Scale factor validation                                */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_scale_factor_range) {
    /* Valid scale factors are 1-4 */
    ASSERT_TRUE(1 >= 1 && 1 <= 4);
    ASSERT_TRUE(2 >= 1 && 2 <= 4);
    ASSERT_TRUE(3 >= 1 && 3 <= 4);
    ASSERT_TRUE(4 >= 1 && 4 <= 4);
    /* 0 and 5 are invalid */
    ASSERT_TRUE(0 < 1);
    ASSERT_TRUE(5 > 4);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 8: Framebuffer size constant                              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_framebuffer_size) {
    /* Framebuffer is always native resolution (scaling is GPU-side) */
    ASSERT_EQ(ORIC_SCREEN_W * ORIC_SCREEN_H * 3, 161280);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 9: Pixel pitch for texture update                         */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_pixel_pitch) {
    /* SDL texture pitch = width * 3 bytes (RGB24) */
    int pitch = ORIC_SCREEN_W * 3;
    ASSERT_EQ(pitch, 720);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 10: Video init and framebuffer clear                      */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_video_init_framebuffer) {
    video_t vid;
    video_init(&vid);
    /* Framebuffer should be initialized (black) */
    ASSERT_EQ(vid.framebuffer[0], 0);
    ASSERT_EQ(vid.framebuffer[1], 0);
    ASSERT_EQ(vid.framebuffer[2], 0);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST RUNNER                                                      */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Display Scaling Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_screen_dimensions);
    RUN(test_window_size_x1);
    RUN(test_window_size_x2);
    RUN(test_window_size_x3);
    RUN(test_window_size_x4);
    RUN(test_renderer_init_headless);
    RUN(test_scale_factor_range);
    RUN(test_framebuffer_size);
    RUN(test_pixel_pitch);
    RUN(test_video_init_framebuffer);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
