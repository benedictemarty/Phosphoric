/**
 * @file test_acia6850.c
 * @brief Motorola MC6850 ACIA unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-21
 *
 * Covers master reset, word-select frame decode, transmit-control (RTS/TIE)
 * decode, the status flags (RDRF/TDRE/DCD/CTS), the RX/TX byte path, IRQ
 * resolution (RIE & RDRF, TIE & TDRE) and the DCD/CTS input pins.
 */

#include <stdio.h>
#include <string.h>
#include "io/acia6850.h"

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
        printf("FAIL\n    %s:%d: expected %d, got %d\n", __FILE__, __LINE__, (int)(b), (int)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); tests_failed++; return; } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); tests_failed++; return; } \
} while(0)

static bool cb_irq;
static int  cb_irq_n;
static void on_irq(void* ud, bool a) { (void)ud; cb_irq = a; cb_irq_n++; }

static acia6850_t acia;

static void setup(void) {
    memset(&acia, 0, sizeof(acia));
    acia.irq_out = on_irq;
    acia6850_init(&acia);
    cb_irq = false; cb_irq_n = 0;
}

/* ═══════════════════════════════════════════════════════════════════════ */

TEST(test_reset_state) {
    setup();
    uint8_t st = acia6850_status(&acia);
    ASSERT_TRUE(st & ACIA6850_SR_TDRE);
    ASSERT_FALSE(st & ACIA6850_SR_RDRF);
    ASSERT_TRUE(st & ACIA6850_SR_DCD);   /* carrier lost */
    ASSERT_TRUE(st & ACIA6850_SR_CTS);   /* not clear */
    ASSERT_EQ(acia.bitmask, 0x7F);
    ASSERT_EQ(acia.framebits, 10);
}

TEST(test_master_reset) {
    setup();
    acia6850_rx_byte(&acia, 0x55);
    ASSERT_TRUE(acia6850_rdrf(&acia));
    bool was_reset = acia6850_control_write(&acia, ACIA6850_CR_MASTER_RST);
    ASSERT_TRUE(was_reset);
    ASSERT_FALSE(acia6850_rdrf(&acia));
    ASSERT_TRUE(acia6850_status(&acia) & ACIA6850_SR_TDRE);
}

TEST(test_word_select_7e1) {
    setup();
    /* $49 = 0100 1001: WS field (bits 4-2) = 010 = 7E1. */
    bool rst = acia6850_control_write(&acia, 0x49);
    ASSERT_FALSE(rst);
    ASSERT_EQ(acia.bitmask, 0x7F);
    ASSERT_EQ(acia.framebits, 10);   /* start + 7 + parity + 1 stop */
}

TEST(test_word_select_8n1) {
    setup();
    /* $55 = 0101 0101: WS field = 101 = 8N1. */
    acia6850_control_write(&acia, 0x55);
    ASSERT_EQ(acia.bitmask, 0xFF);
    ASSERT_EQ(acia.framebits, 10);   /* start + 8 + 0 + 1 stop */
}

TEST(test_rts_decode) {
    setup();
    acia6850_control_write(&acia, 0x49);   /* TC=10 → RTS high → not emitting */
    ASSERT_FALSE(acia6850_rts_low(&acia));
    acia6850_control_write(&acia, 0x09);   /* TC=00 → RTS low → emitting */
    ASSERT_TRUE(acia6850_rts_low(&acia));
}

TEST(test_data_mask_applied) {
    setup();
    acia6850_control_write(&acia, 0x49);   /* 7-bit */
    acia6850_write_data(&acia, 0xC1);      /* 1100 0001 → masked to 0x41 */
    ASSERT_EQ(acia.tdr, 0x41);
    acia6850_rx_byte(&acia, 0xFF);         /* masked to 0x7F */
    ASSERT_EQ(acia.rdr, 0x7F);
}

TEST(test_tx_clears_and_sets_tdre) {
    setup();
    ASSERT_TRUE(acia6850_tdre(&acia));
    acia6850_write_data(&acia, 0x42);
    ASSERT_FALSE(acia6850_tdre(&acia));   /* transmitter busy */
    acia6850_tx_complete(&acia);
    ASSERT_TRUE(acia6850_tdre(&acia));
}

TEST(test_rx_sets_and_clears_rdrf) {
    setup();
    acia6850_rx_byte(&acia, 0x37);
    ASSERT_TRUE(acia6850_rdrf(&acia));
    uint8_t v = acia6850_read_data(&acia);
    ASSERT_EQ(v, 0x37);
    ASSERT_FALSE(acia6850_rdrf(&acia));
}

TEST(test_irq_on_rx_with_rie) {
    setup();
    acia6850_control_write(&acia, 0x95);   /* RIE(bit7)=1, 8N1, TC=00 */
    cb_irq_n = 0;
    acia6850_rx_byte(&acia, 0x10);
    ASSERT_TRUE(cb_irq);
    ASSERT_TRUE(acia6850_status(&acia) & ACIA6850_SR_IRQ);
    acia6850_read_data(&acia);             /* clears RDRF → IRQ drops */
    ASSERT_FALSE(cb_irq);
}

TEST(test_irq_on_tx_with_tie) {
    setup();
    /* TC=01 → RTS low + Tx IRQ enable. TDRE is set after reset → IRQ. */
    cb_irq_n = 0;
    acia6850_control_write(&acia, 0x35);   /* bits6-5 = 01, 8N1 */
    ASSERT_TRUE(cb_irq);                    /* TDRE high + TIE → IRQ */
    acia6850_write_data(&acia, 0x42);      /* clears TDRE → IRQ drops */
    ASSERT_FALSE(cb_irq);
}

TEST(test_dcd_cts_pins) {
    setup();
    acia6850_set_dcd(&acia, true);         /* carrier present → DCD bit clears */
    ASSERT_FALSE(acia6850_status(&acia) & ACIA6850_SR_DCD);
    acia6850_set_cts(&acia, true);         /* clear to send → CTS bit clears */
    ASSERT_FALSE(acia6850_status(&acia) & ACIA6850_SR_CTS);
    acia6850_set_dcd(&acia, false);        /* carrier lost → DCD bit sets */
    ASSERT_TRUE(acia6850_status(&acia) & ACIA6850_SR_DCD);
    /* DCD/CTS do not raise IRQ in this model. */
    ASSERT_FALSE(acia6850_status(&acia) & ACIA6850_SR_IRQ);
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Motorola MC6850 ACIA Tests\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    RUN(test_reset_state);
    RUN(test_master_reset);
    RUN(test_word_select_7e1);
    RUN(test_word_select_8n1);
    RUN(test_rts_decode);
    RUN(test_data_mask_applied);
    RUN(test_tx_clears_and_sets_tdre);
    RUN(test_rx_sets_and_clears_rdrf);
    RUN(test_irq_on_rx_with_rie);
    RUN(test_irq_on_tx_with_tie);
    RUN(test_dcd_cts_pins);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
