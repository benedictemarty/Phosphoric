/**
 * @file test_loci.c
 * @brief Unit tests for LOCI emulation (Sprint 34y skeleton)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../include/io/loci.h"
#include "../../include/utils/logging.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  [%d] %-50s ", ++tests_run, #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", \
               __FILE__, __LINE__, _b, _a); \
        exit(1); \
    } \
} while(0)

/* ── lifecycle ───────────────────────────────────────────────── */

TEST(test_init_zeroes_state) {
    loci_t l; memset(&l, 0xFF, sizeof(l));
    ASSERT_TRUE(loci_init(&l));
    for (size_t i = 0; i < sizeof(l.regs); i++) ASSERT_EQ(l.regs[i], 0);
    ASSERT_EQ(l.active_op, 0);
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

TEST(test_addr_in_mia_filter) {
    ASSERT_TRUE(loci_addr_in_mia(0x03A0));
    ASSERT_TRUE(loci_addr_in_mia(0x03AF));
    ASSERT_TRUE(loci_addr_in_mia(0x03BF));
    ASSERT_TRUE(!loci_addr_in_mia(0x039F));
    ASSERT_TRUE(!loci_addr_in_mia(0x03C0));
}

/* ── bus interface — disabled ────────────────────────────────── */

TEST(test_disabled_read_returns_ff) {
    loci_t l; loci_init(&l);
    l.enabled = false;
    ASSERT_EQ(loci_read(&l, 0x03A0), 0xFF);
    ASSERT_EQ(loci_read(&l, 0x03AF), 0xFF);
}

TEST(test_disabled_write_is_noop) {
    loci_t l; loci_init(&l);
    l.enabled = false;
    loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    ASSERT_EQ(l.regs[LOCI_REG_API_OP], 0);
    ASSERT_EQ(l.op_count[LOCI_OP_RNG_LRAND], 0);
}

/* ── register R/W ────────────────────────────────────────────── */

TEST(test_write_read_passthrough) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03A0, 0x42);
    ASSERT_EQ(loci_read(&l, 0x03A0), 0x42);
    loci_write(&l, 0x03B4, 0xAB);
    ASSERT_EQ(loci_read(&l, 0x03B4), 0xAB);
}

TEST(test_out_of_range_read_ff) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_EQ(loci_read(&l, 0x0399), 0xFF);
    ASSERT_EQ(loci_read(&l, 0x03C0), 0xFF);
}

/* ── API dispatch ────────────────────────────────────────────── */

TEST(test_api_op_dispatches_and_sets_enosys) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    /* errno should be ENOSYS */
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENOSYS);
    /* BUSY cleared after sync dispatch */
    ASSERT_TRUE((l.regs[LOCI_REG_BUSY] & 0x80) == 0);
    ASSERT_EQ(l.op_count[LOCI_OP_OPEN], 1);
}

TEST(test_api_op_none_does_not_dispatch) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_NONE);
    ASSERT_EQ(l.op_count[LOCI_OP_NONE], 0);
}

TEST(test_op_count_increments) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    for (int i = 0; i < 3; i++)
        loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    ASSERT_EQ(l.op_count[LOCI_OP_RNG_LRAND], 3);
}

/* ── xstack ─────────────────────────────────────────────────── */

TEST(test_xstack_push_pop_roundtrip) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AC, 0x10);  /* push 0x10 */
    loci_write(&l, 0x03AC, 0x20);  /* push 0x20 */
    /* Read returns top of stack — 0x20 */
    ASSERT_EQ(loci_read(&l, 0x03AC), 0x20);
}

/* ── 34z: system / RTC / RNG ─────────────────────────────────── */

TEST(test_op_rng_lrand_returns_axsreg) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    /* errno should not be set; high bit cleared (firmware masks 0x7FFFFFFF) */
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    uint8_t sreg_hi = l.regs[LOCI_REG_API_SREG_HI];
    ASSERT_TRUE((sreg_hi & 0x80) == 0);
    ASSERT_TRUE((l.regs[LOCI_REG_BUSY] & 0x80) == 0);
}

TEST(test_op_clock_returns_uptime) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Sleep briefly so uptime isn't 0. */
    struct timespec ts = {0, 20 * 1000 * 1000};   /* 20 ms */
    nanosleep(&ts, NULL);
    loci_write(&l, 0x03AF, LOCI_OP_CLOCK);
    /* Read AXSREG (32-bit) — should be >= 1 (10 ms unit) */
    uint32_t v = l.regs[LOCI_REG_API_A] |
                 ((uint32_t)l.regs[LOCI_REG_API_X]    << 8) |
                 ((uint32_t)l.regs[LOCI_REG_API_SREG] << 16) |
                 ((uint32_t)l.regs[LOCI_REG_API_SREG_HI] << 24);
    ASSERT_TRUE(v >= 1);
}

TEST(test_op_clk_getres_pushes_one_sec) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 0;   /* CLOCK_REALTIME */
    loci_write(&l, 0x03AF, LOCI_OP_CLK_GETRES);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);   /* ax = 0 (OK) */
    /* xstack must hold: uint32 sec=1, int32 nsec=0 — 8 bytes total */
    ASSERT_EQ(LOCI_XSTACK_SIZE - l.xstack_ptr, 8);
    /* Top of stack is the lo byte of sec (1). */
    ASSERT_EQ(l.xstack[l.xstack_ptr], 1);
}

TEST(test_op_clk_gettime_pushes_time) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_CLK_GETTIME);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_EQ(LOCI_XSTACK_SIZE - l.xstack_ptr, 8);   /* uint32+int32 */
}

TEST(test_op_clk_getres_bad_id_returns_einval) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 99;
    loci_write(&l, 0x03AF, LOCI_OP_CLK_GETRES);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EINVAL);
}

TEST(test_op_pix_xreg_no_errno) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_PIX_XREG);
    /* PIX_XREG currently accepts everything silently. */
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
}

TEST(test_xstack_read_pops_byte) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AC, 0x10);
    loci_write(&l, 0x03AC, 0x20);
    /* Top is 0x20; reading pops it. */
    ASSERT_EQ(loci_read(&l, 0x03AC), 0x20);
    /* Next read returns 0x10. */
    ASSERT_EQ(loci_read(&l, 0x03AC), 0x10);
    /* Empty now. */
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

TEST(test_unimplemented_op_still_enosys) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);   /* not implemented in 34z */
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENOSYS);
}

/* ── reset ──────────────────────────────────────────────────── */

TEST(test_reset_clears_state) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    loci_write(&l, 0x03A0, 0x55);
    loci_reset(&l);
    ASSERT_EQ(l.regs[LOCI_REG_API_ERRNO_LO], 0);
    ASSERT_EQ(l.regs[0], 0);
    ASSERT_EQ(l.active_op, 0);
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

int main(void) {
    log_init(LOG_LEVEL_ERROR);
    printf("\n");
    printf("===========================================================\n");
    printf("  Phosphoric LOCI Emulation Tests (Sprint 34y skeleton)\n");
    printf("===========================================================\n\n");

    RUN(test_init_zeroes_state);
    RUN(test_addr_in_mia_filter);
    RUN(test_disabled_read_returns_ff);
    RUN(test_disabled_write_is_noop);
    RUN(test_write_read_passthrough);
    RUN(test_out_of_range_read_ff);
    RUN(test_api_op_dispatches_and_sets_enosys);
    RUN(test_api_op_none_does_not_dispatch);
    RUN(test_op_count_increments);
    RUN(test_xstack_push_pop_roundtrip);
    RUN(test_xstack_read_pops_byte);
    RUN(test_op_pix_xreg_no_errno);
    RUN(test_op_rng_lrand_returns_axsreg);
    RUN(test_op_clock_returns_uptime);
    RUN(test_op_clk_getres_pushes_one_sec);
    RUN(test_op_clk_gettime_pushes_time);
    RUN(test_op_clk_getres_bad_id_returns_einval);
    RUN(test_unimplemented_op_still_enosys);
    RUN(test_reset_clears_state);

    printf("\n===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_run - tests_passed, tests_run);
    printf("===========================================================\n\n");
    return tests_passed == tests_run ? 0 : 1;
}
