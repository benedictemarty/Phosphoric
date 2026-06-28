/**
 * @file test_io.c
 * @brief Comprehensive VIA 6522 and I/O unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-alpha
 */

#include <stdio.h>
#include <string.h>
#include "io/via6522.h"

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
        printf("FAIL\n    %s:%d: expected 0x%X, got 0x%X\n", __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
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

/* ═══════════════════════════════════════════════════════════════════ */
/*  INIT/RESET TESTS                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_via_init) {
    via6522_t via;
    via_init(&via);
    ASSERT_EQ(via.t1_counter, 0);
    ASSERT_EQ(via.ora, 0);
    ASSERT_EQ(via.orb, 0);
    ASSERT_EQ(via.ddra, 0);
    ASSERT_EQ(via.ddrb, 0);
    ASSERT_EQ(via.ifr, 0);
    ASSERT_EQ(via.ier, 0);
}

TEST(test_via_reset) {
    via6522_t via;
    via_init(&via);
    via.ora = 0xFF;
    via.ddra = 0xFF;
    via_reset(&via);
    ASSERT_EQ(via.ora, 0);
    ASSERT_EQ(via.ddra, 0);
    ASSERT_EQ(via.t1_counter, 0xFFFF);
    ASSERT_FALSE(via.t1_running);
    ASSERT_FALSE(via.t2_running);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  REGISTER READ/WRITE TESTS                                        */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ddr_read_write) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_DDRA, 0xFF);
    ASSERT_EQ(via_read(&via, VIA_DDRA), 0xFF);
    via_write(&via, VIA_DDRB, 0xAA);
    ASSERT_EQ(via_read(&via, VIA_DDRB), 0xAA);
}

TEST(test_acr_pcr) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0xC0);
    ASSERT_EQ(via_read(&via, VIA_ACR), 0xC0);
    via_write(&via, VIA_PCR, 0x55);
    ASSERT_EQ(via_read(&via, VIA_PCR), 0x55);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TIMER 1 TESTS                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_timer1_load) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    /* Load timer 1 latch low */
    via_write(&via, VIA_T1CL, 0x00);
    /* Load timer 1 counter high (starts timer) */
    via_write(&via, VIA_T1CH, 0x01);
    ASSERT_TRUE(via.t1_running);
    ASSERT_EQ(via.t1_counter, 0x0100);
}

TEST(test_timer1_one_shot) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.acr &= ~0x40; /* One-shot mode */
    via_write(&via, VIA_T1CL, 0x05);
    via_write(&via, VIA_T1CH, 0x00);
    ASSERT_TRUE(via.t1_running);
    /* Run timer for 10 cycles */
    via_update(&via, 10);
    /* Timer should have fired and stopped */
    ASSERT_FALSE(via.t1_running);
    ASSERT_TRUE(via.ifr & VIA_INT_T1);
}

TEST(test_timer1_free_running) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.acr |= 0x40; /* Free-running mode */
    via_write(&via, VIA_T1CL, 0x0A);
    via_write(&via, VIA_T1CH, 0x00);
    ASSERT_TRUE(via.t1_running);
    /* Run timer to trigger */
    via_update(&via, 15);
    ASSERT_TRUE(via.ifr & VIA_INT_T1);
    /* Timer should still be running (reloaded from latch) */
    ASSERT_TRUE(via.t1_running);
}

TEST(test_timer1_read_clears_ifr) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via_write(&via, VIA_T1CL, 0x02);
    via_write(&via, VIA_T1CH, 0x00);
    via_update(&via, 10);
    ASSERT_TRUE(via.ifr & VIA_INT_T1);
    /* Reading T1CL should clear T1 interrupt flag */
    via_read(&via, VIA_T1CL);
    ASSERT_FALSE(via.ifr & VIA_INT_T1);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TIMER 2 TESTS                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_timer2_one_shot) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.acr &= ~0x20; /* Timer mode (not pulse counting) */
    via_write(&via, VIA_T2CL, 0x05);
    via_write(&via, VIA_T2CH, 0x00);
    ASSERT_TRUE(via.t2_running);
    via_update(&via, 10);
    ASSERT_FALSE(via.t2_running);
    ASSERT_TRUE(via.ifr & VIA_INT_T2);
}

TEST(test_timer2_read_clears_ifr) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via_write(&via, VIA_T2CL, 0x02);
    via_write(&via, VIA_T2CH, 0x00);
    via_update(&via, 10);
    ASSERT_TRUE(via.ifr & VIA_INT_T2);
    via_read(&via, VIA_T2CL);
    ASSERT_FALSE(via.ifr & VIA_INT_T2);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INTERRUPT TESTS                                                   */
/* ═══════════════════════════════════════════════════════════════════ */

static bool test_irq_state = false;
static void test_irq_cb(bool state, void* userdata) {
    (void)userdata;
    test_irq_state = state;
}

TEST(test_ier_set_clear) {
    via6522_t via;
    via_init(&via);
    /* Set T1 interrupt enable */
    via_write(&via, VIA_IER, 0x80 | VIA_INT_T1); /* Bit 7=1 means set */
    ASSERT_TRUE(via.ier & VIA_INT_T1);
    /* Clear T1 interrupt enable */
    via_write(&via, VIA_IER, VIA_INT_T1); /* Bit 7=0 means clear */
    ASSERT_FALSE(via.ier & VIA_INT_T1);
}

TEST(test_irq_callback) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    test_irq_state = false;
    via_set_irq_callback(&via, test_irq_cb, NULL);

    /* Enable T1 interrupt */
    via_write(&via, VIA_IER, 0x80 | VIA_INT_T1);
    /* Start short timer */
    via_write(&via, VIA_T1CL, 0x02);
    via_write(&via, VIA_T1CH, 0x00);
    via_update(&via, 10);
    ASSERT_TRUE(test_irq_state);
}

TEST(test_ifr_write_clears) {
    via6522_t via;
    via_init(&via);
    via.ifr = VIA_INT_T1 | VIA_INT_T2;
    /* Writing 1 bits to IFR clears those flags */
    via_write(&via, VIA_IFR, VIA_INT_T1);
    ASSERT_FALSE(via.ifr & VIA_INT_T1);
    ASSERT_TRUE(via.ifr & VIA_INT_T2);
}

TEST(test_ier_read_bit7) {
    via6522_t via;
    via_init(&via);
    via.ier = 0x40;
    /* Reading IER should always have bit 7 set */
    ASSERT_EQ(via_read(&via, VIA_IER), 0xC0);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  PORT CALLBACK TESTS                                               */
/* ═══════════════════════════════════════════════════════════════════ */

static uint8_t test_porta_val = 0;
static uint8_t test_portb_val = 0;

static uint8_t test_porta_read_cb(void* ud) { (void)ud; return test_porta_val; }
static void test_porta_write_cb(uint8_t val, void* ud) { (void)ud; test_porta_val = val; }
static uint8_t test_portb_read_cb(void* ud) { (void)ud; return test_portb_val; }
static void test_portb_write_cb(uint8_t val, void* ud) { (void)ud; test_portb_val = val; }

TEST(test_port_a_read) {
    via6522_t via;
    via_init(&via);
    via.ddra = 0x00; /* All input */
    /* External device drives IRA (e.g. PSG in READ mode via psg_decode).
     * VIA_ORA returns (ORA & DDRA) | (IRA & ~DDRA) = IRA when DDRA=0. */
    via.ira = 0xAA;
    uint8_t val = via_read(&via, VIA_ORA);
    ASSERT_EQ(val, 0xAA);
}

TEST(test_port_a_write) {
    via6522_t via;
    via_init(&via);
    via_set_port_callbacks(&via, test_porta_read_cb, test_porta_write_cb,
                           test_portb_read_cb, test_portb_write_cb, NULL);
    test_porta_val = 0;
    via_write(&via, VIA_ORA, 0x55);
    ASSERT_EQ(test_porta_val, 0x55);
}

TEST(test_port_b_mixed_ddr) {
    via6522_t via;
    via_init(&via);
    via_set_port_callbacks(&via, test_porta_read_cb, test_porta_write_cb,
                           test_portb_read_cb, test_portb_write_cb, NULL);
    via.ddrb = 0xF0; /* Upper nibble output, lower input */
    via.orb = 0xA0;
    test_portb_val = 0x05;
    uint8_t val = via_read(&via, VIA_ORB);
    /* Output bits from ORB, input bits from callback */
    ASSERT_EQ(val, 0xA5);
}

TEST(test_ora_no_handshake) {
    via6522_t via;
    via_init(&via);
    via.ddra = 0x00;
    /* IRA driven by external device (no CA1 handshake involved). */
    via.ira = 0xCC;
    uint8_t val = via_read(&via, VIA_ORA_NH);
    ASSERT_EQ(val, 0xCC);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TRIGGER TESTS                                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_trigger_ca1) {
    via6522_t via;
    via_init(&via);
    via_trigger_ca1(&via);
    ASSERT_TRUE(via.ifr & VIA_INT_CA1);
}

TEST(test_trigger_ca2) {
    via6522_t via;
    via_init(&via);
    via_trigger_ca2(&via);
    ASSERT_TRUE(via.ifr & VIA_INT_CA2);
}

TEST(test_trigger_cb1) {
    via6522_t via;
    via_init(&via);
    via_trigger_cb1(&via);
    ASSERT_TRUE(via.ifr & VIA_INT_CB1);
}

TEST(test_trigger_cb2) {
    via6522_t via;
    via_init(&via);
    via_trigger_cb2(&via);
    ASSERT_TRUE(via.ifr & VIA_INT_CB2);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  CB1 EDGE DETECTION TESTS                                          */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_cb1_edge_falling) {
    /* PCR bit 4 = 0: interrupt on falling edge */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.pcr &= ~0x10;  /* Falling edge (default) */
    /* CB1 starts high after reset, drive it low */
    via_set_cb1(&via, false);
    ASSERT_TRUE(via.ifr & VIA_INT_CB1);
}

TEST(test_cb1_edge_rising) {
    /* PCR bit 4 = 1: interrupt on rising edge */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.pcr |= 0x10;  /* Rising edge */
    /* Drive low first (no trigger on rising edge mode) */
    via.cb1_pin = false;  /* Force pin low without triggering */
    /* Now drive high: should trigger */
    via_set_cb1(&via, true);
    ASSERT_TRUE(via.ifr & VIA_INT_CB1);
}

TEST(test_cb1_no_trigger_wrong_edge) {
    /* PCR bit 4 = 1 (rising): falling edge should NOT trigger */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.pcr |= 0x10;  /* Rising edge mode */
    /* CB1 starts high, drive low = falling edge */
    via_set_cb1(&via, false);
    ASSERT_FALSE(via.ifr & VIA_INT_CB1);
}

TEST(test_cb1_no_trigger_same_state) {
    /* Setting CB1 to same state should not trigger */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.pcr &= ~0x10;  /* Falling edge */
    /* CB1 starts high, set high again = no transition */
    via_set_cb1(&via, true);
    ASSERT_FALSE(via.ifr & VIA_INT_CB1);
}

TEST(test_cb1_clear_and_retrigger) {
    /* Clear CB1 flag via ORB read, then re-trigger */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.pcr &= ~0x10;  /* Falling edge */
    /* First trigger */
    via_set_cb1(&via, false);
    ASSERT_TRUE(via.ifr & VIA_INT_CB1);
    /* Clear CB1 flag by reading ORB */
    via_read(&via, VIA_ORB);
    ASSERT_FALSE(via.ifr & VIA_INT_CB1);
    /* Re-trigger: drive high then low again */
    via_set_cb1(&via, true);
    via_set_cb1(&via, false);
    ASSERT_TRUE(via.ifr & VIA_INT_CB1);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SHIFT REGISTER TESTS                                              */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_shift_register) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_SR, 0xAA);
    ASSERT_EQ(via_read(&via, VIA_SR), 0xAA);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  REGISTER MASKING TEST                                             */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_register_mask) {
    via6522_t via;
    via_init(&via);
    /* Register addresses should be masked to 4 bits */
    via_write(&via, 0x12, 0xAA); /* Same as VIA_DDRB (0x02) */
    ASSERT_EQ(via_read(&via, 0x12), 0xAA);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TIMER EXACT-ZERO TESTS                                            */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_via_t1_exact_zero) {
    via6522_t via;
    via_init(&via);
    /* Set T1 latch to 2, start timer */
    via_write(&via, VIA_T1CL, 2);
    via_write(&via, VIA_T1CH, 0);  /* starts timer */
    /* Clear IFR T1 flag that may have been set */
    via.ifr = 0;
    /* Update by exactly 2 cycles: counter goes from 2 to 0 */
    via_update(&via, 2);
    /* IFR bit 6 (T1) should be set */
    ASSERT_EQ(via.ifr & 0x40, 0x40);
}

TEST(test_via_t2_exact_zero) {
    via6522_t via;
    via_init(&via);
    /* Set T2 latch to 4 */
    via_write(&via, VIA_T2CL, 4);
    via_write(&via, VIA_T2CH, 0);  /* starts timer */
    /* Clear IFR */
    via.ifr = 0;
    /* ACR bit 5 clear = timer mode (not pulse counting) */
    via.acr &= ~0x20;
    /* Update by exactly 4 cycles: counter goes from 4 to 0 */
    via_update(&via, 4);
    /* IFR bit 5 (T2) should be set */
    ASSERT_EQ(via.ifr & 0x20, 0x20);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SHIFT REGISTER SHIFTING + T2 PULSE COUNTING + CA2/CB2 PINS        */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_sr_shift_out_phi2) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x18);   /* shift OUT under φ2 */
    via_write(&via, VIA_SR, 0xAA);    /* starts the sequence */
    ASSERT_TRUE(via.sr_active);
    /* 8 shifts at one per 2 cycles = 16 cycles → SR interrupt flag set */
    via_update(&via, 16);
    ASSERT_EQ(via.ifr & VIA_INT_SR, VIA_INT_SR);
    ASSERT_FALSE(via.sr_active);
}

TEST(test_sr_shift_in_phi2) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x08);   /* shift IN under φ2 */
    via_set_cb2_input(&via, true);    /* all-ones serial input */
    (void)via_read(&via, VIA_SR);     /* reading SR starts a shift-in sequence */
    ASSERT_TRUE(via.sr_active);
    via_update(&via, 16);
    ASSERT_EQ(via.ifr & VIA_INT_SR, VIA_INT_SR);
    ASSERT_EQ(via.sr, 0xFF);          /* eight 1-bits shifted in */
}

TEST(test_sr_shift_out_external) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x1C);   /* shift OUT under external clock (CB1) */
    via_write(&via, VIA_SR, 0x80);
    /* via_update must NOT shift in external-clock mode */
    via_update(&via, 100);
    ASSERT_FALSE(via.ifr & VIA_INT_SR);
    for (int i = 0; i < 8; i++) via_shift_clock(&via);
    ASSERT_EQ(via.ifr & VIA_INT_SR, VIA_INT_SR);
}

TEST(test_sr_free_running_no_flag) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x10);   /* free-running shift OUT under T2 */
    via_write(&via, VIA_SR, 0xAA);
    via_update(&via, 64);
    /* Free-running mode never sets the SR flag and keeps running. */
    ASSERT_FALSE(via.ifr & VIA_INT_SR);
    ASSERT_TRUE(via.sr_active);
}

TEST(test_t2_pulse_counting) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x20);   /* T2 pulse-counting mode (PB6) */
    via_write(&via, VIA_T2CL, 0x02);
    via_write(&via, VIA_T2CH, 0x00);  /* T2 = 2, running */
    /* φ2 ticks must NOT decrement T2 in pulse mode */
    via_update(&via, 1000);
    ASSERT_FALSE(via.ifr & VIA_INT_T2);
    via_pb6_pulse(&via);              /* 2 → 1 */
    via_pb6_pulse(&via);              /* 1 → 0 */
    ASSERT_FALSE(via.ifr & VIA_INT_T2);
    via_pb6_pulse(&via);              /* 0 → underflow → IRQ */
    ASSERT_EQ(via.ifr & VIA_INT_T2, VIA_INT_T2);
}

TEST(test_ca2_cb2_manual_output) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x0E);   /* CA2 manual output high */
    ASSERT_TRUE(via_get_ca2(&via));
    via_write(&via, VIA_PCR, 0x0C);   /* CA2 manual output low */
    ASSERT_FALSE(via_get_ca2(&via));
    via_write(&via, VIA_PCR, 0xE0);   /* CB2 manual output high */
    ASSERT_TRUE(via_get_cb2(&via));
    via_write(&via, VIA_PCR, 0xC0);   /* CB2 manual output low */
    ASSERT_FALSE(via_get_cb2(&via));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                              */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("Running VIA 6522 I/O tests...\n");
    printf("═══════════════════════════════════════════════════════════\n");

    printf("\n  Init/Reset:\n");
    RUN(test_via_init);
    RUN(test_via_reset);

    printf("\n  Register Read/Write:\n");
    RUN(test_ddr_read_write);
    RUN(test_acr_pcr);

    printf("\n  Timer 1:\n");
    RUN(test_timer1_load);
    RUN(test_timer1_one_shot);
    RUN(test_timer1_free_running);
    RUN(test_timer1_read_clears_ifr);

    printf("\n  Timer 2:\n");
    RUN(test_timer2_one_shot);
    RUN(test_timer2_read_clears_ifr);

    printf("\n  Interrupts:\n");
    RUN(test_ier_set_clear);
    RUN(test_irq_callback);
    RUN(test_ifr_write_clears);
    RUN(test_ier_read_bit7);

    printf("\n  Port Callbacks:\n");
    RUN(test_port_a_read);
    RUN(test_port_a_write);
    RUN(test_port_b_mixed_ddr);
    RUN(test_ora_no_handshake);

    printf("\n  Triggers:\n");
    RUN(test_trigger_ca1);
    RUN(test_trigger_ca2);
    RUN(test_trigger_cb1);
    RUN(test_trigger_cb2);

    printf("\n  CB1 Edge Detection:\n");
    RUN(test_cb1_edge_falling);
    RUN(test_cb1_edge_rising);
    RUN(test_cb1_no_trigger_wrong_edge);
    RUN(test_cb1_no_trigger_same_state);
    RUN(test_cb1_clear_and_retrigger);

    printf("\n  Shift Register:\n");
    RUN(test_shift_register);
    RUN(test_sr_shift_out_phi2);
    RUN(test_sr_shift_in_phi2);
    RUN(test_sr_shift_out_external);
    RUN(test_sr_free_running_no_flag);

    printf("\n  T2 Pulse Counting & CA2/CB2 Pins:\n");
    RUN(test_t2_pulse_counting);
    RUN(test_ca2_cb2_manual_output);

    printf("\n  Register Masking:\n");
    RUN(test_register_mask);

    printf("\n  Timer Exact-Zero:\n");
    RUN(test_via_t1_exact_zero);
    RUN(test_via_t2_exact_zero);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
