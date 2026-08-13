/**
 * @file test_autotype.c
 * @brief Unit tests for the scan-driven --type-keys pacing primitives.
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-07-20
 * @version 1.94.0-alpha
 *
 * Covers include/io/autotype.h:
 *   - autotype_is_new_pass(): keyboard scan-pass boundary detection
 *   - autotype_should_fire(): the additive cycle + scan-pass gate, LOCI
 *     bypass and the anti-stall watchdog.
 */

#include <stdio.h>
#include "io/autotype.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-52s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
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

/* ---- autotype_is_new_pass ------------------------------------------------ */

/* The ROM scanner sweeps columns descending (7->0). A rise = new pass. */
TEST(test_new_pass_on_column_rise) {
    ASSERT_TRUE(autotype_is_new_pass(0, 7));   /* 0 -> 7 : sweep restarted */
    ASSERT_TRUE(autotype_is_new_pass(3, 4));   /* any climb counts */
}

TEST(test_no_pass_on_descending_sweep) {
    ASSERT_FALSE(autotype_is_new_pass(7, 6));
    ASSERT_FALSE(autotype_is_new_pass(1, 0));
}

TEST(test_no_pass_on_same_column) {
    ASSERT_FALSE(autotype_is_new_pass(5, 5));
}

/* The 0xFF sentinel (no previous read) never starts a pass. */
TEST(test_sentinel_prev_col_no_pass) {
    ASSERT_FALSE(autotype_is_new_pass(0xFF, 7));
    ASSERT_FALSE(autotype_is_new_pass(0xFF, 0));
}

/* Early-stop sweep (key found at col C) still detects the next pass at 7. */
TEST(test_new_pass_after_early_stop) {
    ASSERT_TRUE(autotype_is_new_pass(4, 7));   /* stopped at 4, restart at 7 */
}

/* ---- autotype_should_fire ------------------------------------------------ */

/* Cycle schedule not yet reached -> never fires, whatever the passes. */
TEST(test_gate_blocks_before_cycle) {
    ASSERT_FALSE(autotype_should_fire(false, 100, 200, 999, 0));
    ASSERT_FALSE(autotype_should_fire(true,  100, 200, 999, 0));
}

/* ORIC path: cycle reached AND enough scan passes elapsed -> fire. */
TEST(test_gate_fires_with_enough_passes) {
    uint32_t last = 10;
    ASSERT_TRUE(autotype_should_fire(false, 500, 200,
                                     last + AUTOTYPE_MIN_PASS, last));
}

/* ORIC path: cycle reached but scanner hasn't advanced enough -> wait. */
TEST(test_gate_waits_for_scan_passes) {
    uint32_t last = 10;
    ASSERT_FALSE(autotype_should_fire(false, 500, 200,
                                      last + AUTOTYPE_MIN_PASS - 1, last));
}

/* LOCI HID path bypasses the scan gate: cycle reached is sufficient. */
TEST(test_gate_loci_bypasses_passes) {
    ASSERT_TRUE(autotype_should_fire(true, 500, 200, 0, 0));
}

/* Watchdog: a program that never scans still progresses once the cycle
 * counter runs far enough past the schedule. */
TEST(test_gate_watchdog_breaks_stall) {
    uint32_t last = 10;
    int64_t next = 200;
    /* Just before the watchdog margin: still blocked. */
    ASSERT_FALSE(autotype_should_fire(false,
                 next + AUTOTYPE_WATCHDOG_CYCLES - 1, next, last, last));
    /* At/after the margin: fires despite zero new passes. */
    ASSERT_TRUE(autotype_should_fire(false,
                 next + AUTOTYPE_WATCHDOG_CYCLES, next, last, last));
}

int main(void) {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  Auto-type scan-driven pacing tests\n");
    printf("═══════════════════════════════════════════════════════════\n");

    printf("\n  autotype_is_new_pass — scan-pass boundary:\n");
    RUN(test_new_pass_on_column_rise);
    RUN(test_no_pass_on_descending_sweep);
    RUN(test_no_pass_on_same_column);
    RUN(test_sentinel_prev_col_no_pass);
    RUN(test_new_pass_after_early_stop);

    printf("\n  autotype_should_fire — cycle + scan-pass gate:\n");
    RUN(test_gate_blocks_before_cycle);
    RUN(test_gate_fires_with_enough_passes);
    RUN(test_gate_waits_for_scan_passes);
    RUN(test_gate_loci_bypasses_passes);
    RUN(test_gate_watchdog_breaks_stall);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
