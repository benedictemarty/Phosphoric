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

/* Helper: build PCR value from CA2 mode (bits 1-3) */
static uint8_t pcr_ca2(uint8_t ca2_mode) {
    return (uint8_t)((ca2_mode << 1) & 0x0E);
}

/* Helper: simulate one STROBE pulse (forced low → forced high) */
static void strobe_byte(oric_printer_t* p, uint8_t data) {
    uint8_t pcr_idle = pcr_ca2(CA2_FORCED_HIGH);
    uint8_t pcr_low  = pcr_ca2(CA2_FORCED_LOW);
    uint8_t pcr_high = pcr_ca2(CA2_FORCED_HIGH);

    /* Step 1: STROBE asserted (CA2 forced low) */
    oric_printer_check_strobe(p, pcr_idle, pcr_low, data);
    /* Step 2: STROBE released (CA2 forced high) → byte captured */
    oric_printer_check_strobe(p, pcr_low, pcr_high, data);
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

    /* Go directly to forced high without forced low first */
    uint8_t pcr_idle = pcr_ca2(CA2_FORCED_HIGH);
    oric_printer_check_strobe(&printer, pcr_idle, pcr_idle, 'Z');
    ASSERT_EQ(printer.byte_count, 0);

    oric_printer_close(&printer);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 7: CA2 mode bits extraction                                */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_printer_ca2_bits) {
    /* Verify CA2 mode extraction from PCR:
     * PCR bits 1-3 = CA2 control.
     * CA2_FORCED_LOW = 0x06 → PCR bits 1-3 = 110
     * CA2_FORCED_HIGH = 0x07 → PCR bits 1-3 = 111 */
    ASSERT_EQ(CA2_FORCED_LOW, 0x06);
    ASSERT_EQ(CA2_FORCED_HIGH, 0x07);

    /* PCR for forced low: bits 1-3 = 110 → PCR = 0x0C */
    uint8_t pcr_low = pcr_ca2(CA2_FORCED_LOW);
    ASSERT_EQ((pcr_low >> 1) & 0x07, CA2_FORCED_LOW);

    /* PCR for forced high: bits 1-3 = 111 → PCR = 0x0E */
    uint8_t pcr_high = pcr_ca2(CA2_FORCED_HIGH);
    ASSERT_EQ((pcr_high >> 1) & 0x07, CA2_FORCED_HIGH);
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
    RUN(test_printer_ca2_bits);
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
