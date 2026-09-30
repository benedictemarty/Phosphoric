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

/* Le sous-dépassement du 6522 n'est PAS l'atteinte de zéro : c'est le passage de
 * $0000 à $FFFF, un cycle plus tard (V2-E3). Avec N=2 : deux décomptes (2→1, 1→0)
 * puis le cycle de sous-dépassement qui pose le flag. */
TEST(test_via_t1_underflow_one_cycle_after_zero) {
    /* Le compteur chargé ne décompte qu'au cycle SUIVANT l'écriture de T1C-H
     * (vrai VIC-20, viavarious) ; le drapeau tombe un cycle après zéro. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_T1CL, 2);
    via_write(&via, VIA_T1CH, 0);  /* démarre le timer */
    via.ifr = 0;
    via_update(&via, 3);           /* cycle de l'écriture, puis 2 → 0 */
    ASSERT_EQ(via.t1_counter, 0x0000);
    ASSERT_EQ(via.ifr & 0x40, 0x00);   /* …le flag n'est pas encore posé */
    via_update(&via, 1);           /* $0000 → $FFFF : sous-dépassement */
    ASSERT_EQ(via.ifr & 0x40, 0x40);
}

/* Même mécanique pour Timer 2 (one-shot). */
TEST(test_via_t2_underflow_one_cycle_after_zero) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_T2CL, 4);
    via_write(&via, VIA_T2CH, 0);  /* démarre le timer (décompte au cycle suivant) */
    via.ifr = 0;
    via.acr &= ~0x20;              /* mode timer, pas comptage d'impulsions */
    via_update(&via, 5);
    ASSERT_EQ(via.ifr & 0x20, 0x00);
    via_update(&via, 1);
    ASSERT_EQ(via.ifr & 0x20, 0x20);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  VECTEURS DE TIMING DU VIA (V2-E3 / US3.3)                          */
/*                                                                    */
/*  La propriété déterminante du Timer 1 est sa PÉRIODE en mode        */
/*  continu : N+2 cycles (N décomptes + 1 cycle de sous-dépassement    */
/*  + 1 cycle de rechargement). C'est elle qui fixe la fréquence des   */
/*  IRQ, des sons et des impulsions cassette sur PB7. L'ancienne       */
/*  implémentation donnait N, soit 2 cycles de trop peu par période —  */
/*  0,02 % à 100 Hz, mais 20 % pour N=10.                              */
/* ═══════════════════════════════════════════════════════════════════ */

/* Mesure la période entre deux poses du flag T1 en mode continu. */
static int measure_t1_period(int n) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x40);            /* Timer 1 continu */
    via_write(&via, VIA_T1CL, n & 0xFF);
    via_write(&via, VIA_T1CH, (n >> 8) & 0xFF);
    int first = -1;
    for (int c = 1; c <= 4 * (n + 8); c++) {
        via_update(&via, 1);
        if (via.ifr & 0x40) {
            via_write(&via, VIA_IFR, 0x40);    /* efface le flag */
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

/* Le time-out one-shot tombe N+1 cycles après l'écriture de T1C-H : N décomptes
 * puis le cycle de sous-dépassement. (La datasheet parle de N+1,5 : le demi-cycle
 * n'est pas modélisable à la granularité du cycle entier — voir docs/ACCURACY.md.) */
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
        ASSERT_EQ(fired, ns[i] + 2);   /* N+2 : décompte au cycle suivant l'écriture */
    }
}

/* One-shot : le compteur continue de décompter après le time-out (datasheet p.8,
 * l'hôte lit le temps écoulé), mais il ne tire plus. */
TEST(test_via_t1_oneshot_counter_keeps_running_without_refiring) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x00);
    via_write(&via, VIA_T1CL, 3);
    via_write(&via, VIA_T1CH, 0);
    for (int c = 0; c < 5; c++) via_update(&via, 1);    /* tir au 5e cycle (décompte au cycle suivant l'écriture) */
    ASSERT_EQ(via.ifr & 0x40, 0x40);
    via_write(&via, VIA_IFR, 0x40);                     /* acquitte */
    uint16_t after_fire = via.t1_counter;
    for (int c = 0; c < 7; c++) via_update(&via, 1);     /* période latch + 2 = 5 : pas un multiple */
    ASSERT_TRUE(via.t1_counter != after_fire);           /* il compte toujours */
    ASSERT_EQ(via.ifr & 0x40, 0x00);                     /* mais ne retire pas */
}

/* PB7 en signal carré : une bascule par sous-dépassement, donc une période
 * électrique de 2 × (N+2) cycles. C'est ce qui cadence l'écriture cassette. */
TEST(test_via_pb7_square_wave_period) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_DDRB, 0x80);           /* PB7 en sortie */
    via_write(&via, VIA_ACR, 0xC0);            /* T1 continu + sortie PB7 */
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
    ASSERT_EQ(second_edge - first_edge, 12);   /* N+2 entre deux fronts */
}

/* Handshake CA2 en mode impulsion (PCR 101) : CA2 passe bas à l'accès ORA puis
 * remonte après exactement UN cycle (datasheet, « pulse output »). */
TEST(test_via_ca2_pulse_lasts_one_cycle) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_PCR, 0x0A);        /* CA2 = sortie impulsion */
    via_write(&via, VIA_ORA, 0x55);        /* l'accès déclenche l'impulsion */
    ASSERT_FALSE(via_get_ca2(&via));       /* bas pendant le cycle */
    via_update(&via, 1);
    ASSERT_TRUE(via_get_ca2(&via));        /* remonté au cycle suivant */
}

/* La lecture de T1C-L/T1C-H doit rendre la valeur courante du compteur : c'est
 * ainsi qu'un programme mesure le temps écoulé depuis le démarrage du timer. */
/* ── VIA mesurée sur vrai 6522 (VICE testprogs VIC-20, report Neo6502Vic20) ── */

TEST(test_via_t2_mode_switch_next_cycle) {
    /* viavarious via1 G, via2, via9 : le mode de T2 choisi par l'ACR bit 5
     * (φ2 / impulsions PB6) ne prend effet qu'au cycle suivant, dans les deux sens. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_T2CL, 0x00);
    via_write(&via, VIA_T2CH, 0x10);
    via_update(&via, 3);
    ASSERT_EQ(via.t2_counter, 0x0FFE);
    via_write(&via, VIA_ACR, 0x20);          /* → comptage PB6 */
    via_update(&via, 1);
    ASSERT_EQ(via.t2_counter, 0x0FFD);       /* ce cycle compte encore φ2 */
    via_update(&via, 4);
    ASSERT_EQ(via.t2_counter, 0x0FFD);       /* puis plus rien */
    via_write(&via, VIA_ACR, 0x00);          /* → φ2 */
    via_update(&via, 1);
    ASSERT_EQ(via.t2_counter, 0x0FFD);       /* pas encore */
    via_update(&via, 1);
    ASSERT_EQ(via.t2_counter, 0x0FFC);
}

TEST(test_via_t2_8bit_when_sr_uses_t2) {
    /* viavarious via20/via21 : registre à décalage cadencé par T2 → T2 sur
     * 8 bits : octet bas rechargé du latch bas (période latch + 2), octet haut
     * décrémenté à chaque sous-dépassement, drapeau T2 unique au passage à $FFFF. */
    static const uint16_t expect[] = { 0x0102, 0x0101, 0x0100, 0x00FF, 0x0002, 0x0001,
                                       0x0000, 0xFFFF, 0xFF02, 0xFF01, 0xFF00, 0xFEFF };
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x04);          /* SR en entrée cadencé par T2 */
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
    ASSERT_EQ(first, 8);                     /* au passage des 16 bits à $FFFF */
    ASSERT_EQ(flags, 1);                     /* une seule fois */
}

TEST(test_via_pb7_toggle_rules) {
    /* viavarious via10-13, via_pb7 : bascule à 0 à l'écriture de T1C-H, à 1
     * quand l'ACR bit 7 passe de 0 à 1, et change d'état à chaque interruption
     * de T1 (continu : signal carré de période 2 × (latch + 2)). */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    via_write(&via, VIA_T1CL, 3);
    via_write(&via, VIA_T1CH, 0);
    ASSERT_FALSE(via.pb7_pin);
    via_write(&via, VIA_ACR, 0xC0);          /* continu + sortie PB7 : 0 → 1 */
    ASSERT_TRUE(via_get_pb7(&via));
    via_write(&via, VIA_T1CH, 0);            /* relance : → 0 */
    static const char want[] = "0000111110000011";
    for (int i = 0; i < 16; i++) {
        via_update(&via, 1);
        ASSERT_EQ(via_get_pb7(&via) ? '1' : '0', want[i]);
    }
}

TEST(test_via_sr_any_access_starts_and_acr0_clears_flag) {
    /* via_sr : toute lecture OU écriture du SR démarre une séquence, quel que
     * soit le sens ; ACR = 000 tient le drapeau SR à 0. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x18);          /* sortie φ2 */
    (void)via_read(&via, VIA_SR);            /* une lecture suffit */
    ASSERT_TRUE(via.sr_active);
    via_init(&via);
    via_write(&via, VIA_ACR, 0x08);          /* entrée φ2 */
    via_write(&via, VIA_SR, 0x00);           /* une écriture suffit */
    via_update(&via, 17);
    ASSERT_EQ(via.ifr & VIA_INT_SR, VIA_INT_SR);
    via_write(&via, VIA_ACR, 0x00);
    ASSERT_EQ(via.ifr & VIA_INT_SR, 0);
}

TEST(test_via_t1_oneshot_reloads_from_latch) {
    /* Mesuré sur un vrai VIC-20 (viavarious via1 ; VICE calcule T1 modulo
     * latch + 2 dans les deux modes) : en one-shot aussi, le compteur se
     * recharge depuis le latch à chaque sous-dépassement ; seule l'IRQ est unique. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x00);
    via_write(&via, VIA_T1CL, 3);
    via_write(&via, VIA_T1CH, 0);
    via_update(&via, 5);                    /* 3 → 0, puis $FFFF : tir */
    ASSERT_EQ(via.t1_counter, 0xFFFF);
    ASSERT_EQ(via.ifr & 0x40, 0x40);
    via_update(&via, 1);
    ASSERT_EQ(via.t1_counter, 3);           /* rechargé depuis le latch */
    via_write(&via, VIA_IFR, 0x40);
    via_update(&via, 4);                    /* 3 → 0 puis deuxième passage à $FFFF */
    ASSERT_EQ(via.t1_counter, 0xFFFF);
    ASSERT_EQ(via.ifr & 0x40, 0x00);        /* sans nouvelle interruption */
}

TEST(test_via_t1_counter_readback) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x00);
    via_write(&via, VIA_T1CL, 0xE8);       /* N = 1000 */
    via_write(&via, VIA_T1CH, 0x03);
    via_update(&via, 400);                 /* cycle d'écriture + 399 décomptes */
    uint8_t lo = via_read(&via, VIA_T1CL);
    uint8_t hi = via_read(&via, VIA_T1CH);
    ASSERT_EQ((hi << 8) | lo, 1000 - 399);
    /* Lire T1C-L efface le flag T1, lire T1C-H ne l'efface pas (datasheet). */
    via_update(&via, 602);                 /* sous-dépassement */
    ASSERT_EQ(via.ifr & 0x40, 0x40);
    (void)via_read(&via, VIA_T1CH);
    ASSERT_EQ(via.ifr & 0x40, 0x40);       /* T1C-H : flag intact */
    (void)via_read(&via, VIA_T1CL);
    ASSERT_EQ(via.ifr & 0x40, 0x00);       /* T1C-L : flag effacé */
}

/* Intégration : Timer 1 continu programmé à la période d'une trame PAL doit
 * produire exactement une interruption par trame sur 50 trames — le test qui
 * attrape une dérive de période, même d'un seul cycle. */
TEST(test_via_t1_frame_rate_over_50_frames) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_IER, 0xC0);            /* autorise T1 */
    via_write(&via, VIA_ACR, 0x40);            /* continu */
    /* Une trame PAL = 19968 cycles ; période N+2 → N = 19966. */
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
    /* Séquence de 16 demi-périodes de CB1 ; en φ2, premier événement 1 cycle
     * après l'accès au SR (vrai VIC-20, via_sr). Sortie aux états pairs. */
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x18);   /* shift OUT under φ2 */
    via_write(&via, VIA_SR, 0xAA);    /* starts the sequence */
    ASSERT_TRUE(via.sr_active);
    via_update(&via, 16);
    ASSERT_EQ(via.ifr & VIA_INT_SR, 0);
    via_update(&via, 1);              /* 16e événement → drapeau SR */
    ASSERT_EQ(via.ifr & VIA_INT_SR, VIA_INT_SR);
    ASSERT_FALSE(via.sr_active);
    ASSERT_EQ(via.sr, 0xAA);          /* 8 rotations : l'octet revient */
}

TEST(test_sr_shift_in_phi2) {
    via6522_t via;
    via_init(&via);
    via_write(&via, VIA_ACR, 0x08);   /* shift IN under φ2 */
    via_set_cb2_input(&via, true);    /* all-ones serial input */
    (void)via_read(&via, VIA_SR);     /* reading SR starts a shift-in sequence */
    ASSERT_TRUE(via.sr_active);
    via_update(&via, 17);             /* 1 cycle de délai + 16 demi-périodes */
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
    /* Mesuré sur un vrai VIC-20 (VICE testprogs viavarious via10-13, report de
     * Neo6502Vic20) : PB7 est la sortie de T1 dès que ACR bit7 = 1, même avec
     * DDRB bit7 = 0 ; la bascule vaut 1 au RESET, passe à 0 à chaque écriture de
     * T1C-H et change d'état à chaque interruption T1. */
    via6522_t via;
    via_init(&via);
    via_reset(&via);
    ASSERT_TRUE(via.pb7_pin);         /* bascule à 1 au RESET */
    via_write(&via, VIA_ACR, 0x80);   /* one-shot, sortie PB7 */
    via_write(&via, VIA_DDRB, 0x00);  /* DDRB.7 = 0 : sortie T1 quand même */
    via_write(&via, VIA_T1CL, 0x05);
    via_write(&via, VIA_T1CH, 0x00);
    ASSERT_FALSE(via_get_pb7(&via));  /* T1C-H écrit → 0 */
    via_update(&via, 10);             /* sous-dépassement → 1 */
    ASSERT_TRUE(via_get_pb7(&via));
    via_write(&via, VIA_ACR, 0x00);   /* ACR bit7 = 0 : PB7 redevient une broche */
    ASSERT_TRUE(via_get_pb7(&via));   /* entrée, tirée haut */
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

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
