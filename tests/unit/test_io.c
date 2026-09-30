/* SPDX-License-Identifier: EUPL-1.2 */
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

/* The 6522 underflow is NOT reaching zero: it is the transition from
 * $0000 to $FFFF, one cycle later (V2-E3). With N=2: two countdowns (2→1, 1→0)
 * then the underflow cycle that sets the flag. */
TEST(test_via_t1_underflow_one_cycle_after_zero) {
    /* The loaded counter only starts counting down on the cycle AFTER the
     * write to T1C-H (real VIC-20, viavarious); the flag falls one cycle after zero. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_T1CL, 2);
    via_write(&via, VIA_T1CH, 0);  /* starts the timer */
    via.ifr = 0;
    via_update(&via, 3);           /* write cycle, then 2 → 0 */
    ASSERT_EQ(via.t1_counter, 0x0000);
    ASSERT_EQ(via.ifr & 0x40, 0x00);   /* …the flag is not set yet */
    via_update(&via, 1);           /* $0000 → $FFFF: underflow */
    ASSERT_EQ(via.ifr & 0x40, 0x40);
}

/* Same mechanics for Timer 2 (one-shot). */
TEST(test_via_t2_underflow_one_cycle_after_zero) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_T2CL, 4);
    via_write(&via, VIA_T2CH, 0);  /* starts the timer (countdown on the next cycle) */
    via.ifr = 0;
    via.acr &= ~0x20;              /* timer mode, not pulse counting */
    via_update(&via, 5);
    ASSERT_EQ(via.ifr & 0x20, 0x00);
    via_update(&via, 1);
    ASSERT_EQ(via.ifr & 0x20, 0x20);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  VIA TIMING VECTORS (V2-E3 / US3.3)                                 */
/*                                                                    */
/*  The defining property of Timer 1 is its PERIOD in free-run         */
/*  mode: N+2 cycles (N countdowns + 1 underflow cycle                 */
/*  + 1 reload cycle). It sets the frequency of the                    */
/*  IRQs, of sounds and of cassette pulses on PB7. The old             */
/*  implementation gave N, i.e. 2 cycles too few per period —          */
/*  0.02 % at 100 Hz, but 20 % for N=10.                               */
/* ═══════════════════════════════════════════════════════════════════ */

/* Measures the period between two settings of the T1 flag in free-run mode. */
static int measure_t1_period(int n) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x40);            /* Timer 1 free-run */
    via_write(&via, VIA_T1CL, n & 0xFF);
    via_write(&via, VIA_T1CH, (n >> 8) & 0xFF);
    int first = -1;
    for (int c = 1; c <= 4 * (n + 8); c++) {
        via_update(&via, 1);
        if (via.ifr & 0x40) {
            via_write(&via, VIA_IFR, 0x40);    /* clear the flag */
            if (first < 0) first = c;
            else return c - first;
        }
    }
    return -1;
}

TEST(test_via_t1_freerun_period_is_n_plus_2) {
    static const int ns[] = { 1, 2, 5, 10, 100, 999, 9998 };
    for (unsigned i = 0; i < sizeof(ns) / sizeof(ns[0]); i++)
        ASSERT_EQ(measure_t1_period(ns[i]), ns[i] + 2);
}

/* The one-shot time-out falls N+1 cycles after the T1C-H write: N countdowns
 * then the underflow cycle. (The datasheet says N+1.5: the half cycle
 * cannot be modelled at whole-cycle granularity — see docs/ACCURACY.md.) */
TEST(test_via_t1_oneshot_timeout_cycle) {
    static const int ns[] = { 1, 5, 50, 500 };
    for (unsigned i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
        via6522_t via;
        via_init(&via);
        via_write(&via, VIA_ACR, 0x00);        /* one-shot */
        via_write(&via, VIA_T1CL, ns[i] & 0xFF);
        via_write(&via, VIA_T1CH, (ns[i] >> 8) & 0xFF);
        int fired = -1;
        for (int c = 1; c <= ns[i] + 8 && fired < 0; c++) {
            via_update(&via, 1);
            if (via.ifr & 0x40) fired = c;
        }
        ASSERT_EQ(fired, ns[i] + 2);   /* N+2: countdown starts on the cycle after the write */
    }
}

/* One-shot: the counter keeps counting down after the time-out (datasheet p.8,
 * the host reads the elapsed time), but it no longer fires. */
TEST(test_via_t1_oneshot_counter_keeps_running_without_refiring) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x00);
    via_write(&via, VIA_T1CL, 3);
    via_write(&via, VIA_T1CH, 0);
    for (int c = 0; c < 5; c++) via_update(&via, 1);    /* fires on the 5th cycle (countdown starts on the cycle after the write) */
    ASSERT_EQ(via.ifr & 0x40, 0x40);
    via_write(&via, VIA_IFR, 0x40);                     /* acknowledge */
    uint16_t after_fire = via.t1_counter;
    for (int c = 0; c < 7; c++) via_update(&via, 1);     /* period latch + 2 = 5: not a multiple */
    ASSERT_TRUE(via.t1_counter != after_fire);           /* it still counts */
    ASSERT_EQ(via.ifr & 0x40, 0x00);                     /* but does not fire again */
}

/* PB7 as a square wave: one toggle per underflow, hence an electrical
 * period of 2 × (N+2) cycles. This is what clocks the cassette write. */
TEST(test_via_pb7_square_wave_period) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_DDRB, 0x80);           /* PB7 as output */
    via_write(&via, VIA_ACR, 0xC0);            /* T1 free-run + PB7 output */
    via_write(&via, VIA_T1CL, 10);
    via_write(&via, VIA_T1CH, 0);
    bool prev = via_get_pb7(&via);
    int edges = 0, first_edge = -1, second_edge = -1;
    for (int c = 1; c <= 200; c++) {
        via_update(&via, 1);
        bool now = via_get_pb7(&via);
        if (now != prev) {
            edges++;
            if (first_edge < 0) first_edge = c;
            else if (second_edge < 0) second_edge = c;
            prev = now;
        }
    }
    ASSERT_TRUE(edges >= 2);
    ASSERT_EQ(second_edge - first_edge, 12);   /* N+2 between two edges */
}

/* CA2 handshake in pulse mode (PCR 101): CA2 goes low on the ORA access then
 * goes back high after exactly ONE cycle (datasheet, « pulse output »). */
TEST(test_via_ca2_pulse_lasts_one_cycle) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x0A);        /* CA2 = pulse output */
    via_write(&via, VIA_ORA, 0x55);        /* the access triggers the pulse */
    ASSERT_FALSE(via_get_ca2(&via));       /* low during the cycle */
    via_update(&via, 1);
    ASSERT_TRUE(via_get_ca2(&via));        /* back high on the next cycle */
}

/* Reading T1C-L/T1C-H must return the current counter value: this is
 * how a program measures the time elapsed since the timer was started. */
/* ── VIA measured on a real 6522 (VICE VIC-20 testprogs, ported from Neo6502Vic20) ── */

TEST(test_via_t2_mode_switch_next_cycle) {
    /* viavarious via1 G, via2, via9: the T2 mode selected by ACR bit 5
     * (φ2 / PB6 pulses) only takes effect on the next cycle, in both directions. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_T2CL, 0x00);
    via_write(&via, VIA_T2CH, 0x10);
    via_update(&via, 3);
    ASSERT_EQ(via.t2_counter, 0x0FFE);
    via_write(&via, VIA_ACR, 0x20);          /* → PB6 counting */
    via_update(&via, 1);
    ASSERT_EQ(via.t2_counter, 0x0FFD);       /* this cycle still counts φ2 */
    via_update(&via, 4);
    ASSERT_EQ(via.t2_counter, 0x0FFD);       /* then nothing more */
    via_write(&via, VIA_ACR, 0x00);          /* → φ2 */
    via_update(&via, 1);
    ASSERT_EQ(via.t2_counter, 0x0FFD);       /* not yet */
    via_update(&via, 1);
    ASSERT_EQ(via.t2_counter, 0x0FFC);
}

TEST(test_via_t2_8bit_when_sr_uses_t2) {
    /* viavarious via20/via21: shift register clocked by T2 → 8-bit T2:
     * low byte reloaded from the low latch (period latch + 2), high byte
     * decremented on each underflow, single T2 flag when rolling over to $FFFF. */
    static const uint16_t expect[] = { 0x0102, 0x0101, 0x0100, 0x00FF, 0x0002, 0x0001,
                                       0x0000, 0xFFFF, 0xFF02, 0xFF01, 0xFF00, 0xFEFF };
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x04);          /* SR input clocked by T2 */
    via_write(&via, VIA_T2CL, 0x02);
    via_write(&via, VIA_T2CH, 0x01);
    via.ifr = 0;
    int flags = 0, first = -1;
    for (int i = 0; i < 12; i++) {
        via_update(&via, 1);
        ASSERT_EQ(via.t2_counter, expect[i]);
        if ((via.ifr & VIA_INT_T2) && first < 0) first = i + 1;
        if (via.ifr & VIA_INT_T2) { flags++; via.ifr &= (uint8_t)~VIA_INT_T2; }
    }
    ASSERT_EQ(first, 8);                     /* when the 16 bits roll over to $FFFF */
    ASSERT_EQ(flags, 1);                     /* only once */
}

TEST(test_via_pb7_toggle_rules) {
    /* viavarious via10-13, via_pb7: flip-flop goes to 0 on a write to T1C-H, to 1
     * when ACR bit 7 goes from 0 to 1, and toggles on every T1
     * interrupt (free-run: square wave of period 2 × (latch + 2)). */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via_write(&via, VIA_T1CL, 3);
    via_write(&via, VIA_T1CH, 0);
    ASSERT_FALSE(via.pb7_pin);
    via_write(&via, VIA_ACR, 0xC0);          /* free-run + PB7 output: 0 → 1 */
    ASSERT_TRUE(via_get_pb7(&via));
    via_write(&via, VIA_T1CH, 0);            /* restart: → 0 */
    static const char want[] = "0000111110000011";
    for (int i = 0; i < 16; i++) {
        via_update(&via, 1);
        ASSERT_EQ(via_get_pb7(&via) ? '1' : '0', want[i]);
    }
}

TEST(test_via_sr_any_access_starts_and_acr0_clears_flag) {
    /* via_sr: any read OR write of the SR starts a sequence, whatever the
     * direction; ACR = 000 holds the SR flag at 0. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x18);          /* φ2 output */
    (void)via_read(&via, VIA_SR);            /* a read is enough */
    ASSERT_TRUE(via.sr_active);
    via_init(&via);
    via_write(&via, VIA_ACR, 0x08);          /* φ2 input */
    via_write(&via, VIA_SR, 0x00);           /* a write is enough */
    via_update(&via, 17);
    ASSERT_EQ(via.ifr & VIA_INT_SR, VIA_INT_SR);
    via_write(&via, VIA_ACR, 0x00);
    ASSERT_EQ(via.ifr & VIA_INT_SR, 0);
}

TEST(test_via_t1_oneshot_reloads_from_latch) {
    /* Measured on a real VIC-20 (viavarious via1; VICE computes T1 modulo
     * latch + 2 in both modes): in one-shot mode too, the counter reloads
     * from the latch on every underflow; only the IRQ is single. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x00);
    via_write(&via, VIA_T1CL, 3);
    via_write(&via, VIA_T1CH, 0);
    via_update(&via, 5);                    /* 3 → 0, then $FFFF: fires */
    ASSERT_EQ(via.t1_counter, 0xFFFF);
    ASSERT_EQ(via.ifr & 0x40, 0x40);
    via_update(&via, 1);
    ASSERT_EQ(via.t1_counter, 3);           /* reloaded from the latch */
    via_write(&via, VIA_IFR, 0x40);
    via_update(&via, 4);                    /* 3 → 0 then second rollover to $FFFF */
    ASSERT_EQ(via.t1_counter, 0xFFFF);
    ASSERT_EQ(via.ifr & 0x40, 0x00);        /* with no new interrupt */
}

TEST(test_via_t1_counter_readback) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x00);
    via_write(&via, VIA_T1CL, 0xE8);       /* N = 1000 */
    via_write(&via, VIA_T1CH, 0x03);
    via_update(&via, 400);                 /* write cycle + 399 countdowns */
    uint8_t lo = via_read(&via, VIA_T1CL);
    uint8_t hi = via_read(&via, VIA_T1CH);
    ASSERT_EQ((hi << 8) | lo, 1000 - 399);
    /* Reading T1C-L clears the T1 flag, reading T1C-H does not (datasheet). */
    via_update(&via, 602);                 /* underflow */
    ASSERT_EQ(via.ifr & 0x40, 0x40);
    (void)via_read(&via, VIA_T1CH);
    ASSERT_EQ(via.ifr & 0x40, 0x40);       /* T1C-H: flag intact */
    (void)via_read(&via, VIA_T1CL);
    ASSERT_EQ(via.ifr & 0x40, 0x00);       /* T1C-L: flag cleared */
}

/* Integration: a free-run Timer 1 programmed to the period of a PAL frame must
 * produce exactly one interrupt per frame over 50 frames — the test that
 * catches a period drift, even of a single cycle. */
TEST(test_via_t1_frame_rate_over_50_frames) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_IER, 0xC0);            /* enable T1 */
    via_write(&via, VIA_ACR, 0x40);            /* free-run */
    /* One PAL frame = 19968 cycles; period N+2 → N = 19966. */
    const int n = 19966;
    via_write(&via, VIA_T1CL, n & 0xFF);
    via_write(&via, VIA_T1CH, (n >> 8) & 0xFF);
    int fires = 0;
    for (int c = 0; c < 50 * 19968; c++) {
        via_update(&via, 1);
        if (via.ifr & 0x40) { via_write(&via, VIA_IFR, 0x40); fires++; }
    }
    ASSERT_EQ(fires, 50);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SHIFT REGISTER SHIFTING + T2 PULSE COUNTING + CA2/CB2 PINS        */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_sr_shift_out_phi2) {
    /* Sequence of 16 CB1 half-periods; in φ2 mode, first event 1 cycle
     * after the SR access (real VIC-20, via_sr). Output on even states. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x18);   /* shift OUT under φ2 */
    via_write(&via, VIA_SR, 0xAA);    /* starts the sequence */
    ASSERT_TRUE(via.sr_active);
    via_update(&via, 16);
    ASSERT_EQ(via.ifr & VIA_INT_SR, 0);
    via_update(&via, 1);              /* 16th event → SR flag */
    ASSERT_EQ(via.ifr & VIA_INT_SR, VIA_INT_SR);
    ASSERT_FALSE(via.sr_active);
    ASSERT_EQ(via.sr, 0xAA);          /* 8 rotations: the byte comes back */
}

TEST(test_sr_shift_in_phi2) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x08);   /* shift IN under φ2 */
    via_set_cb2_input(&via, true);    /* all-ones serial input */
    (void)via_read(&via, VIA_SR);     /* reading SR starts a shift-in sequence */
    ASSERT_TRUE(via.sr_active);
    via_update(&via, 17);             /* 1 delay cycle + 16 half-periods */
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

TEST(test_ca2_handshake_output) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x08);   /* CA2 = 100 handshake output */
    via.ca2_pin = true;
    via_read(&via, VIA_ORA);          /* "data taken": CA2 drops low */
    ASSERT_FALSE(via_get_ca2(&via));
    via_update(&via, 100);
    ASSERT_FALSE(via_get_ca2(&via));  /* stays low: no pulse in this mode */
    via_set_ca1(&via, false);         /* CA1 active edge (falling, PCR b0=0) */
    ASSERT_TRUE(via_get_ca2(&via));   /* "data ready" restores CA2 high */
    ASSERT_TRUE(via.ifr & VIA_INT_CA1);
}

TEST(test_ca2_pulse_output) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x0A);   /* CA2 = 101 pulse output */
    via.ca2_pin = true;
    via_write(&via, VIA_ORA, 0x42);   /* access pulses CA2 low... */
    ASSERT_FALSE(via_get_ca2(&via));
    via_update(&via, 1);              /* ...for exactly one φ2 cycle */
    ASSERT_TRUE(via_get_ca2(&via));
}

TEST(test_cb2_write_handshake_only) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x80);   /* CB2 = 100 handshake output */
    via.cb2_pin = true;
    via_read(&via, VIA_ORB);          /* reading ORB must NOT drop CB2 */
    ASSERT_TRUE(via_get_cb2(&via));
    via_write(&via, VIA_ORB, 0x00);   /* writing ORB does */
    ASSERT_FALSE(via_get_cb2(&via));
    via_set_cb1(&via, false);         /* CB1 active edge restores CB2 */
    ASSERT_TRUE(via_get_cb2(&via));
}

TEST(test_ca2_cb2_input_edges) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x00);   /* CA2/CB2 input, falling edge */
    via_set_ca2_input(&via, true);
    ASSERT_FALSE(via.ifr & VIA_INT_CA2);
    via_set_ca2_input(&via, false);   /* falling edge → flag */
    ASSERT_TRUE(via.ifr & VIA_INT_CA2);
    ASSERT_FALSE(via_get_ca2(&via));  /* input modes read the pin */
    via_set_cb2_input(&via, true);
    ASSERT_FALSE(via.ifr & VIA_INT_CB2);
    via_set_cb2_input(&via, false);
    ASSERT_TRUE(via.ifr & VIA_INT_CB2);
    /* Rising-edge variants (CA2 mode 010 = 0x04, CB2 mode 010 = 0x40) */
    via_write(&via, VIA_IFR, VIA_INT_CA2 | VIA_INT_CB2);
    via_write(&via, VIA_PCR, 0x44);
    via_set_ca2_input(&via, true);
    via_set_cb2_input(&via, true);
    ASSERT_TRUE(via.ifr & VIA_INT_CA2);
    ASSERT_TRUE(via.ifr & VIA_INT_CB2);
}

TEST(test_ca2_independent_not_cleared_by_ora) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x02);   /* CA2 = 001 independent, falling */
    via_set_ca2_input(&via, true);
    via_set_ca2_input(&via, false);
    ASSERT_TRUE(via.ifr & VIA_INT_CA2);
    via_read(&via, VIA_ORA);          /* independent mode: flag SURVIVES */
    ASSERT_TRUE(via.ifr & VIA_INT_CA2);
    via_write(&via, VIA_IFR, VIA_INT_CA2);
    via_write(&via, VIA_PCR, 0x00);   /* plain input mode */
    via_set_ca2_input(&via, true);
    via_set_ca2_input(&via, false);
    ASSERT_TRUE(via.ifr & VIA_INT_CA2);
    via_read(&via, VIA_ORA);          /* non-independent: cleared by access */
    ASSERT_FALSE(via.ifr & VIA_INT_CA2);
}

TEST(test_porta_latching_on_ca1) {
    via6522_t via;
    via_init(&via);
    via.ddra = 0x00;                  /* all inputs */
    via.ira = 0xAA;                   /* PSG bus value (pins = 0xFF) */
    via_write(&via, VIA_ACR, 0x01);   /* enable PA latching */
    via_set_ca1(&via, false);         /* active edge captures 0xAA */
    via.ira = 0x55;                   /* pins change afterwards... */
    ASSERT_EQ(via_read(&via, VIA_ORA), 0xAA);   /* ...read stays latched */
    via_set_ca1(&via, true);
    via_set_ca1(&via, false);         /* next edge reloads the latch */
    ASSERT_EQ(via_read(&via, VIA_ORA), 0x55);
    via_write(&via, VIA_ACR, 0x00);   /* latching off: live pins again */
    via.ira = 0x0F;
    ASSERT_EQ(via_read(&via, VIA_ORA), 0x0F);
}

/* After a one-shot Timer 1 times out, the counter keeps decrementing (so the
 * host can read the time since the interrupt) but the flag must NOT re-arm
 * (datasheet p.8). */
TEST(test_timer1_one_shot_counter_continues) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.acr &= ~0x40;                 /* one-shot */
    via_write(&via, VIA_T1CL, 0x64);  /* latch = 100 */
    via_write(&via, VIA_T1CH, 0x00);
    via_update(&via, 105);            /* past time-out */
    ASSERT_FALSE(via.t1_running);
    ASSERT_TRUE(via.ifr & VIA_INT_T1);
    via_read(&via, VIA_T1CL);         /* clear the T1 flag */
    ASSERT_FALSE(via.ifr & VIA_INT_T1);
    uint16_t c1 = via.t1_counter;
    via_update(&via, 20);
    ASSERT_TRUE(via.t1_counter != c1);   /* still counting */
    ASSERT_FALSE(via.ifr & VIA_INT_T1);  /* one-shot does not re-fire */
}

/* Same for Timer 2 (one-shot timer mode). */
TEST(test_timer2_one_shot_counter_continues) {
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via.acr &= ~0x20;                 /* timer mode */
    via_write(&via, VIA_T2CL, 0x64);
    via_write(&via, VIA_T2CH, 0x00);
    via_update(&via, 105);
    ASSERT_FALSE(via.t2_running);
    ASSERT_TRUE(via.ifr & VIA_INT_T2);
    via_read(&via, VIA_T2CL);         /* clear the T2 flag */
    ASSERT_FALSE(via.ifr & VIA_INT_T2);
    uint16_t c1 = via.t2_counter;
    via_update(&via, 20);
    ASSERT_TRUE(via.t2_counter != c1);
    ASSERT_FALSE(via.ifr & VIA_INT_T2);
}

/* PB7 is the Timer-1 output only when BOTH DDRB bit7 and ACR bit7 are set
 * (datasheet p.9); with DDRB bit7 = 0, PB7 stays a normal port pin. */
TEST(test_pb7_timer_output_ignores_ddrb7) {
    /* Measured on a real VIC-20 (VICE testprogs viavarious via10-13, ported from
     * Neo6502Vic20): PB7 is the T1 output as soon as ACR bit7 = 1, even with
     * DDRB bit7 = 0; the flip-flop is 1 on RESET, goes to 0 on every write to
     * T1C-H and toggles on every T1 interrupt. */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    ASSERT_TRUE(via.pb7_pin);         /* flip-flop at 1 on RESET */
    via_write(&via, VIA_ACR, 0x80);   /* one-shot, PB7 output */
    via_write(&via, VIA_DDRB, 0x00);  /* DDRB.7 = 0: T1 output anyway */
    via_write(&via, VIA_T1CL, 0x05);
    via_write(&via, VIA_T1CH, 0x00);
    ASSERT_FALSE(via_get_pb7(&via));  /* T1C-H written → 0 */
    via_update(&via, 10);             /* underflow → 1 */
    ASSERT_TRUE(via_get_pb7(&via));
    via_write(&via, VIA_ACR, 0x00);   /* ACR bit7 = 0: PB7 becomes a plain pin again */
    ASSERT_TRUE(via_get_pb7(&via));   /* input, pulled high */
}

/* Lazy path (via_tick, ported from Neo6502Vic20 US-31): two VIAs receive the
 * same pseudo-random sequence of accesses, one stepped (via_update(1) every
 * cycle), the other through via_tick. Every read, the /IRQ line, PB7, CA2 and
 * CB2 must match on every cycle, and the full state at the end. */
static unsigned lazy_irq[2];
static void lazy_irq_cb(bool state, void* ud) { lazy_irq[(int)(size_t)ud] = state; }
static uint32_t lazy_rng = 0x12345678u;
static uint32_t lazy_rand(void) {
    lazy_rng ^= lazy_rng << 13; lazy_rng ^= lazy_rng >> 17; lazy_rng ^= lazy_rng << 5;
    return lazy_rng;
}

TEST(test_via_lazy_matches_stepwise) {
    static const uint8_t regs[] = { VIA_T1CL, VIA_T1CH, VIA_T1LL, VIA_T1LH, VIA_T2CL,
                                    VIA_T2CH, VIA_SR, VIA_ACR, VIA_PCR, VIA_IFR,
                                    VIA_IER, VIA_ORB, VIA_ORA, VIA_DDRB };
    via6522_t a, b;
    via_init(&a); via_init(&b);
    via_set_irq_callback(&a, lazy_irq_cb, (void*)0);
    via_set_irq_callback(&b, lazy_irq_cb, (void*)1);
    via_reset(&a); via_reset(&b);
    lazy_irq[0] = lazy_irq[1] = 0;
    for (long cyc = 0; cyc < 2000000; cyc++) {
        uint32_t r = lazy_rand();
        if ((r & 0x3FF) == 0) {              /* rare access: let the timers run */
            uint8_t reg = regs[(r >> 10) % sizeof regs];
            if (reg == VIA_ACR || reg == VIA_PCR || reg == VIA_IER) {
                uint8_t v = (uint8_t)(r >> 16);
                via_write(&a, reg, v); via_write(&b, reg, v);
            } else if (r & 0x80000000u) {
                uint8_t v = (uint8_t)(r >> 16);
                via_write(&a, reg, v); via_write(&b, reg, v);
            } else {
                ASSERT_EQ(via_read(&b, reg), via_read(&a, reg));
            }
        } else if ((r & 0xFFF) == 0x400) {  /* CB1 edge (external SR clock, PB latch) */
            bool lvl = (r >> 12) & 1;
            via_set_cb1(&a, lvl); via_set_cb1(&b, lvl);
        } else if ((r & 0xFFF) == 0x800) {
            bool lvl = (r >> 12) & 1;
            via_set_ca1(&a, lvl); via_set_ca1(&b, lvl);
        }
        via_update(&a, 1);
        via_tick(&b);
        ASSERT_EQ(lazy_irq[1], lazy_irq[0]);
        ASSERT_EQ(via_get_pb7(&b), via_get_pb7(&a));
        ASSERT_EQ(via_get_ca2(&b), via_get_ca2(&a));
        ASSERT_EQ(via_get_cb2(&b), via_get_cb2(&a));
    }
    via_sync(&b);
    ASSERT_EQ(b.t1_counter, a.t1_counter);
    ASSERT_EQ(b.t2_counter, a.t2_counter);
    ASSERT_EQ(b.ifr, a.ifr);
    ASSERT_EQ(b.sr, a.sr);
    ASSERT_EQ(b.sr_count, a.sr_count);
    ASSERT_EQ(b.t1_active, a.t1_active);
    ASSERT_EQ(b.t2_active, a.t2_active);
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
    RUN(test_timer1_one_shot_counter_continues);
    RUN(test_timer1_free_running);
    RUN(test_timer1_read_clears_ifr);
    RUN(test_pb7_timer_output_ignores_ddrb7);

    printf("\n  Timer 2:\n");
    RUN(test_timer2_one_shot);
    RUN(test_timer2_one_shot_counter_continues);
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
    RUN(test_ca2_handshake_output);
    RUN(test_ca2_pulse_output);
    RUN(test_cb2_write_handshake_only);
    RUN(test_ca2_cb2_input_edges);
    RUN(test_ca2_independent_not_cleared_by_ora);
    RUN(test_porta_latching_on_ca1);

    printf("\n  Register Masking:\n");
    RUN(test_register_mask);

    printf("\n  Timer Exact-Zero:\n");
    RUN(test_via_t1_underflow_one_cycle_after_zero);
    RUN(test_via_t2_underflow_one_cycle_after_zero);

    printf("\n  Vecteurs de timing du VIA (V2-E3):\n");
    RUN(test_via_t1_freerun_period_is_n_plus_2);
    RUN(test_via_t1_oneshot_timeout_cycle);
    RUN(test_via_t1_oneshot_counter_keeps_running_without_refiring);
    RUN(test_via_pb7_square_wave_period);
    RUN(test_via_ca2_pulse_lasts_one_cycle);
    RUN(test_via_t1_oneshot_reloads_from_latch);
    RUN(test_via_t2_mode_switch_next_cycle);
    RUN(test_via_t2_8bit_when_sr_uses_t2);
    RUN(test_via_pb7_toggle_rules);
    RUN(test_via_sr_any_access_starts_and_acr0_clears_flag);
    RUN(test_via_t1_counter_readback);
    RUN(test_via_t1_frame_rate_over_50_frames);
    RUN(test_via_lazy_matches_stepwise);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
