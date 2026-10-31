/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_printer.c
 * @brief Centronics printer interface unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.7.0-alpha
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/printer.h"

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

#define TEST_FILE "/tmp/oric1_test_printer.txt"

/* Helper: one STROBE pulse on PB4, as the ROM does (2.31.4): PB4 low, then
 * back high, which latches the byte. Returns the ACK request of the release. */
static bool strobe_byte(oric_printer_t* p, uint8_t data) {
    (void)oric_printer_check_pb4(p, true, false, data);   /* STROBE asserted */
    return oric_printer_check_pb4(p, false, true, data);  /* released → latched */
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 1: Init state — disabled                                  */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_init) {
    oric_printer_t printer;
    oric_printer_init(&printer);
    ASSERT_FALSE(oric_printer_is_active(&printer));
    ASSERT_EQ(printer.byte_count, 0);
    ASSERT_FALSE(printer.strobe_low);
    ASSERT_TRUE(printer.output == NULL);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 2: Open and close file                                     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_open_close) {
    oric_printer_t printer;
    oric_printer_init(&printer);

    ASSERT_TRUE(oric_printer_open(&printer, TEST_FILE));
    ASSERT_TRUE(oric_printer_is_active(&printer));
    ASSERT_EQ(printer.byte_count, 0);

    oric_printer_close(&printer);
    ASSERT_FALSE(oric_printer_is_active(&printer));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 3: Single byte via STROBE                                  */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_single_byte) {
    oric_printer_t printer;
    oric_printer_init(&printer);
    oric_printer_open(&printer, TEST_FILE);

    strobe_byte(&printer, 'A');
    ASSERT_EQ(printer.byte_count, 1);

    oric_printer_close(&printer);

    /* Verify file content */
    FILE* fp = fopen(TEST_FILE, "r");
    ASSERT_TRUE(fp != NULL);
    int ch = fgetc(fp);
    ASSERT_EQ(ch, 'A');
    fclose(fp);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 4: Multiple bytes (string)                                 */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_string) {
    oric_printer_t printer;
    oric_printer_init(&printer);
    oric_printer_open(&printer, TEST_FILE);

    const char* msg = "HELLO\n";
    for (int i = 0; msg[i]; i++) {
        strobe_byte(&printer, (uint8_t)msg[i]);
    }
    ASSERT_EQ(printer.byte_count, 6);

    oric_printer_close(&printer);

    /* Verify file content */
    FILE* fp = fopen(TEST_FILE, "r");
    ASSERT_TRUE(fp != NULL);
    char buf[32];
    char* ret = fgets(buf, sizeof(buf), fp);
    ASSERT_TRUE(ret != NULL);
    ASSERT_EQ(strcmp(buf, "HELLO\n"), 0);
    fclose(fp);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 5: No output when printer disabled                         */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_disabled_no_output) {
    oric_printer_t printer;
    oric_printer_init(&printer);

    /* Don't open file — printer is disabled */
    strobe_byte(&printer, 'X');
    ASSERT_EQ(printer.byte_count, 0);
    ASSERT_FALSE(oric_printer_is_active(&printer));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 6: STROBE sequence required — CA2 high without prior low   */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_no_strobe_without_low) {
    oric_printer_t printer;
    oric_printer_init(&printer);
    oric_printer_open(&printer, TEST_FILE);

    /* PB4 stays high, or rises without a prior low level: nothing latched */
    ASSERT_FALSE(oric_printer_check_pb4(&printer, true, true, 'Z'));
    ASSERT_FALSE(oric_printer_check_pb4(&printer, false, true, 'Z'));
    ASSERT_EQ(printer.byte_count, 0);

    oric_printer_close(&printer);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 7: STROBE is PB4 and a latched byte asks for the ACK       */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_pb4_ack) {
    /* The ROMs pull PB4 (VIA port B bit 4) low then high (Atmos $F5C1,
     * ORIC-1 $F57B); the caller pulses CA1 when check_pb4 returns true. */
    ASSERT_EQ(PRINTER_STROBE_PB4, 0x10);
    oric_printer_t printer;
    oric_printer_init(&printer);
    oric_printer_open(&printer, TEST_FILE);
    ASSERT_FALSE(oric_printer_check_pb4(&printer, true, false, 'A'));  /* low: not yet */
    ASSERT_TRUE(oric_printer_check_pb4(&printer, false, true, 'A'));   /* release: ACK */
    ASSERT_EQ(printer.byte_count, 1);
    ASSERT_FALSE(oric_printer_check_pb4(&printer, true, true, 'B'));   /* no new strobe */
    ASSERT_EQ(printer.byte_count, 1);
    oric_printer_close(&printer);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 8: Flush forces write to disk                              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_flush) {
    oric_printer_t printer;
    oric_printer_init(&printer);
    oric_printer_open(&printer, TEST_FILE);

    strobe_byte(&printer, 'Q');
    oric_printer_flush(&printer);

    /* File should be readable immediately after flush */
    FILE* fp = fopen(TEST_FILE, "r");
    ASSERT_TRUE(fp != NULL);
    int ch = fgetc(fp);
    ASSERT_EQ(ch, 'Q');
    fclose(fp);

    oric_printer_close(&printer);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 9: Open replaces previous file                             */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_reopen) {
    oric_printer_t printer;
    oric_printer_init(&printer);
    oric_printer_open(&printer, TEST_FILE);

    strobe_byte(&printer, 'A');
    strobe_byte(&printer, 'B');
    ASSERT_EQ(printer.byte_count, 2);

    /* Reopen: should reset byte_count and create new file */
    oric_printer_open(&printer, TEST_FILE);
    ASSERT_EQ(printer.byte_count, 0);
    ASSERT_TRUE(oric_printer_is_active(&printer));

    strobe_byte(&printer, 'C');
    oric_printer_close(&printer);

    /* File should only contain 'C' (previous content overwritten) */
    FILE* fp = fopen(TEST_FILE, "r");
    ASSERT_TRUE(fp != NULL);
    int ch = fgetc(fp);
    ASSERT_EQ(ch, 'C');
    ch = fgetc(fp);
    ASSERT_EQ(ch, EOF);
    fclose(fp);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 10: LPRINT simulation (typical BASIC output)               */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_lprint_simulation) {
    oric_printer_t printer;
    oric_printer_init(&printer);
    oric_printer_open(&printer, TEST_FILE);

    /* Simulate LPRINT "10 PRINT" followed by CR LF
     * (typical LLIST output from BASIC) */
    const char* line = "10 PRINT\r\n";
    for (int i = 0; line[i]; i++) {
        strobe_byte(&printer, (uint8_t)line[i]);
    }
    ASSERT_EQ(printer.byte_count, 10);

    oric_printer_close(&printer);

    /* Verify file */
    FILE* fp = fopen(TEST_FILE, "r");
    ASSERT_TRUE(fp != NULL);
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    ASSERT_EQ(n, 10);
    ASSERT_EQ(memcmp(buf, "10 PRINT\r\n", 10), 0);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST RUNNER                                                      */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Centronics Printer Interface Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_printer_init);
    RUN(test_printer_open_close);
    RUN(test_printer_single_byte);
    RUN(test_printer_string);
    RUN(test_printer_disabled_no_output);
    RUN(test_printer_no_strobe_without_low);
    RUN(test_printer_pb4_ack);
    RUN(test_printer_flush);
    RUN(test_printer_reopen);
    RUN(test_printer_lprint_simulation);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    /* Cleanup test file */
    remove(TEST_FILE);

    return tests_failed > 0 ? 1 : 0;
}
