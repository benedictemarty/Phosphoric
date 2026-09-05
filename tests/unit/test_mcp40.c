/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_mcp40.c
 * @brief MCP-40 4-color pen plotter unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.8.0-alpha
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/mcp40.h"

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

#define TEST_BMP "/tmp/oric1_test_mcp40.bmp"

/** Feed a string to the MCP-40 command parser */
static void send_string(mcp40_t* mcp, const char* str) {
    for (int i = 0; str[i]; i++) {
        mcp40_receive_byte(mcp, (uint8_t)str[i]);
    }
}

/** Get pixel RGB at plotter coordinates (x, y) */
static void get_pixel(const mcp40_t* mcp, int x, int y,
                       uint8_t* r, uint8_t* g, uint8_t* b) {
    int fy = (MCP40_HEIGHT - 1) - y;
    int offset = (fy * MCP40_WIDTH + x) * 3;
    *r = mcp->framebuffer[offset];
    *g = mcp->framebuffer[offset + 1];
    *b = mcp->framebuffer[offset + 2];
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 1: Init state — white paper, pen at origin                 */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_init) {
    mcp40_t mcp;
    mcp40_init(&mcp);
    ASSERT_EQ(mcp.pen_x, 0);
    ASSERT_EQ(mcp.pen_y, 0);
    ASSERT_EQ(mcp.color, MCP40_BLACK);
    ASSERT_EQ(mcp.line_type, MCP40_LINE_SOLID);
    ASSERT_EQ(mcp.char_size, 1);
    ASSERT_FALSE(mcp.dirty);

    /* Paper should be white */
    uint8_t r, g, b;
    get_pixel(&mcp, 0, 0, &r, &g, &b);
    ASSERT_EQ(r, 0xFF);
    ASSERT_EQ(g, 0xFF);
    ASSERT_EQ(b, 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 2: Home command                                            */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_home) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    /* Move pen somewhere */
    send_string(&mcp, "M100,200\n");
    ASSERT_EQ(mcp.pen_x, 100);
    ASSERT_EQ(mcp.pen_y, 200);

    /* Home should return to origin */
    send_string(&mcp, "H\n");
    ASSERT_EQ(mcp.pen_x, 0);
    ASSERT_EQ(mcp.pen_y, 0);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 3: Move (pen up)                                           */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_move) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    send_string(&mcp, "M240,200\n");
    ASSERT_EQ(mcp.pen_x, 240);
    ASSERT_EQ(mcp.pen_y, 200);

    /* Paper should still be white at intermediate points (pen up) */
    uint8_t r, g, b;
    get_pixel(&mcp, 120, 100, &r, &g, &b);
    ASSERT_EQ(r, 0xFF);
    ASSERT_EQ(g, 0xFF);
    ASSERT_EQ(b, 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 4: Draw line                                               */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_draw_line) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    /* Draw horizontal line from (0,0) to (100,0) */
    send_string(&mcp, "D100,0\n");
    ASSERT_EQ(mcp.pen_x, 100);
    ASSERT_EQ(mcp.pen_y, 0);
    ASSERT_TRUE(mcp.dirty);
    ASSERT_EQ(mcp.line_count, 1);

    /* Pixel at (50,0) should be black (pen color default) */
    uint8_t r, g, b;
    get_pixel(&mcp, 50, 0, &r, &g, &b);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(g, 0);
    ASSERT_EQ(b, 0);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 5: Color selection                                         */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_color) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    /* Select red pen */
    send_string(&mcp, "J3\n");
    ASSERT_EQ(mcp.color, MCP40_RED);

    /* Draw a line */
    send_string(&mcp, "D50,0\n");

    /* Check pixel color is red */
    uint8_t r, g, b;
    get_pixel(&mcp, 25, 0, &r, &g, &b);
    ASSERT_EQ(r, 200);
    ASSERT_EQ(g, 0);
    ASSERT_EQ(b, 0);

    /* Select blue */
    send_string(&mcp, "J1\n");
    ASSERT_EQ(mcp.color, MCP40_BLUE);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 6: Multiple coordinates in one command                     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_multi_coords) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    /* Draw triangle: (0,0) → (100,0) → (50,100) → back via separate cmd */
    send_string(&mcp, "D100,0,50,100\n");
    ASSERT_EQ(mcp.pen_x, 50);
    ASSERT_EQ(mcp.pen_y, 100);
    ASSERT_EQ(mcp.line_count, 2);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 7: Character printing                                      */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_print_char) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    send_string(&mcp, "M10,10\n");
    send_string(&mcp, "PA\n");
    ASSERT_EQ(mcp.char_count, 1);
    ASSERT_TRUE(mcp.dirty);

    /* Pen should have advanced */
    ASSERT_TRUE(mcp.pen_x > 10);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 8: Line type                                               */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_line_type) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    send_string(&mcp, "L2\n");
    ASSERT_EQ(mcp.line_type, MCP40_LINE_DASH2);

    send_string(&mcp, "L0\n");
    ASSERT_EQ(mcp.line_type, MCP40_LINE_SOLID);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 9: BMP export                                              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_export_bmp) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    /* Draw a colored line */
    send_string(&mcp, "J2\n");  /* green */
    send_string(&mcp, "D200,150\n");

    ASSERT_TRUE(mcp40_export_bmp(&mcp, TEST_BMP));

    /* Verify BMP file header */
    FILE* fp = fopen(TEST_BMP, "rb");
    ASSERT_TRUE(fp != NULL);
    uint8_t magic[2];
    size_t n = fread(magic, 1, 2, fp);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(magic[0], 'B');
    ASSERT_EQ(magic[1], 'M');
    fclose(fp);
    remove(TEST_BMP);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 10: Command chaining without newlines                      */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_mcp40_command_chain) {
    mcp40_t mcp;
    mcp40_init(&mcp);

    /* Commands separated by next command letter (no CR needed) */
    send_string(&mcp, "J1M50,50D100,50\n");
    ASSERT_EQ(mcp.color, MCP40_BLUE);
    ASSERT_EQ(mcp.pen_x, 100);
    ASSERT_EQ(mcp.pen_y, 50);
    ASSERT_EQ(mcp.line_count, 1);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST RUNNER                                                      */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  MCP-40 4-Color Plotter Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_mcp40_init);
    RUN(test_mcp40_home);
    RUN(test_mcp40_move);
    RUN(test_mcp40_draw_line);
    RUN(test_mcp40_color);
    RUN(test_mcp40_multi_coords);
    RUN(test_mcp40_print_char);
    RUN(test_mcp40_line_type);
    RUN(test_mcp40_export_bmp);
    RUN(test_mcp40_command_chain);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
