/**
 * @file test_pia6821.c
 * @brief Motorola MC6821 PIA unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-21
 *
 * Covers DDR/OR access, output-pin resolution, input reads, the CA1/CB1 and
 * CA2/CB2 interrupt inputs (enable + edge select + flag + clear-on-read), the
 * CA2/CB2 output modes (set/reset, pulse, handshake) and the callback wiring.
 */

#include <stdio.h>
#include <string.h>
#include "io/pia6821.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-52s", #name); \
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

/* ── Callback capture state ── */
static uint8_t cb_porta, cb_portb;
static int     cb_porta_n, cb_portb_n;
static bool    cb_ca2, cb_cb2, cb_irqa, cb_irqb;
static int     cb_ca2_n, cb_cb2_n, cb_irqa_n, cb_irqb_n;

static void on_porta(void* ud, uint8_t d) { (void)ud; cb_porta = d; cb_porta_n++; }
static void on_portb(void* ud, uint8_t d) { (void)ud; cb_portb = d; cb_portb_n++; }
static void on_ca2(void* ud, bool l)      { (void)ud; cb_ca2 = l;  cb_ca2_n++; }
static void on_cb2(void* ud, bool l)      { (void)ud; cb_cb2 = l;  cb_cb2_n++; }
static void on_irqa(void* ud, bool a)     { (void)ud; cb_irqa = a; cb_irqa_n++; }
static void on_irqb(void* ud, bool a)     { (void)ud; cb_irqb = a; cb_irqb_n++; }

static pia6821_t pia;

static void setup(void) {
    memset(&pia, 0, sizeof(pia));
    pia.port_a_out = on_porta;
    pia.port_b_out = on_portb;
    pia.ca2_out    = on_ca2;
    pia.cb2_out    = on_cb2;
    pia.irqa_out   = on_irqa;
    pia.irqb_out   = on_irqb;
    pia6821_init(&pia);
    pia6821_reset(&pia);
    cb_porta = cb_portb = 0;
    cb_porta_n = cb_portb_n = cb_ca2_n = cb_cb2_n = cb_irqa_n = cb_irqb_n = 0;
}

/* ═══════════════════════════════════════════════════════════════════════ */

TEST(test_reset_state) {
    setup();
    ASSERT_EQ(pia6821_read(&pia, PIA_RS_CRA), 0);
    ASSERT_EQ(pia6821_read(&pia, PIA_RS_CRB), 0);
    /* CR bit2 = 0 after reset → the data location addresses DDR (also 0). */
    ASSERT_EQ(pia6821_read(&pia, PIA_RS_PRA), 0);
    ASSERT_FALSE(pia6821_irqa(&pia));
    ASSERT_FALSE(pia6821_irqb(&pia));
}

TEST(test_ddr_or_select) {
    setup();
    /* CR bit2 = 0 → write DDRA. */
    pia6821_write(&pia, PIA_RS_PRA, 0xF0);
    ASSERT_EQ(pia6821_read(&pia, PIA_RS_PRA), 0xF0);   /* reads back DDRA */
    /* Flip CR bit2 → data location now the OR / port. */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_DDR_SELECT);
    pia6821_write(&pia, PIA_RS_PRA, 0xAA);             /* write ORA */
    /* Output pins = ORA & DDRA = 0xAA & 0xF0 = 0xA0. */
    ASSERT_EQ(pia6821_port_a_output(&pia), 0xA0);
}

TEST(test_input_read_mix) {
    setup();
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_DDR_SELECT);
    /* Lower nibble input, upper nibble output. */
    /* (re-select DDR to set direction) */
    pia6821_write(&pia, PIA_RS_CRA, 0);                /* CR bit2=0 → DDR */
    pia6821_write(&pia, PIA_RS_PRA, 0xF0);             /* DDRA: hi=out, lo=in */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_DDR_SELECT);/* CR bit2=1 → port */
    pia6821_write(&pia, PIA_RS_PRA, 0xFF);             /* ORA = 0xFF */
    pia6821_set_port_a_input(&pia, 0x0A);              /* peripheral drives lo nibble */
    /* read = (ORA & DDRA) | (IN & ~DDRA) = 0xF0 | 0x0A = 0xFA */
    ASSERT_EQ(pia6821_read(&pia, PIA_RS_PRA), 0xFA);
}

TEST(test_port_a_callback) {
    setup();
    pia6821_write(&pia, PIA_RS_CRA, 0);
    pia6821_write(&pia, PIA_RS_PRA, 0xFF);   /* DDRA all output */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_DDR_SELECT);
    cb_porta_n = 0;
    pia6821_write(&pia, PIA_RS_PRA, 0x5C);   /* ORA */
    ASSERT_TRUE(cb_porta_n >= 1);
    ASSERT_EQ(cb_porta, 0x5C);
}

TEST(test_ca1_irq_falling) {
    setup();
    /* Enable CA1 IRQ, active on falling edge (bit1 = 0). */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_IRQ1_ENABLE);
    pia6821_set_ca1(&pia, true);     /* high, no edge yet */
    ASSERT_FALSE(pia6821_irqa(&pia));
    pia6821_set_ca1(&pia, false);    /* falling edge → active */
    ASSERT_TRUE(pia6821_irqa(&pia));
    ASSERT_TRUE(pia6821_read(&pia, PIA_RS_CRA) & PIA_CR_IRQ1_FLAG);
    /* Reading the port (CR bit2 must be 1) clears the flag and drops IRQ. */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_IRQ1_ENABLE | PIA_CR_DDR_SELECT);
    pia6821_read(&pia, PIA_RS_PRA);
    ASSERT_FALSE(pia6821_irqa(&pia));
    ASSERT_FALSE(pia6821_read(&pia, PIA_RS_CRA) & PIA_CR_IRQ1_FLAG);
}

TEST(test_ca1_irq_rising_edge_select) {
    setup();
    /* Enable CA1 IRQ, active on rising edge (bit1 = 1). */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_IRQ1_ENABLE | PIA_CR_IRQ1_EDGE);
    pia6821_set_ca1(&pia, false);
    pia6821_set_ca1(&pia, true);     /* rising → active */
    ASSERT_TRUE(pia6821_irqa(&pia));
}

TEST(test_ca1_flag_without_enable) {
    setup();
    /* No enable: the flag still latches but IRQ stays low. */
    pia6821_write(&pia, PIA_RS_CRA, 0);
    pia6821_set_ca1(&pia, true);
    pia6821_set_ca1(&pia, false);    /* falling */
    ASSERT_TRUE(pia6821_read(&pia, PIA_RS_CRA) & PIA_CR_IRQ1_FLAG);
    ASSERT_FALSE(pia6821_irqa(&pia));
}

TEST(test_ca2_input_irq) {
    setup();
    /* CA2 as input (bit5=0), IRQ2 enable (bit3), falling edge (bit4=0). */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_C2_LOW);
    pia6821_set_ca2(&pia, true);
    pia6821_set_ca2(&pia, false);    /* falling → IRQ2 */
    ASSERT_TRUE(pia6821_read(&pia, PIA_RS_CRA) & PIA_CR_IRQ2_FLAG);
    ASSERT_TRUE(pia6821_irqa(&pia));
}

TEST(test_ca2_output_set_reset) {
    setup();
    /* CA2 output (bit5=1), set/reset mode (bit4=1), level = bit3. */
    cb_ca2_n = 0;
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_C2_OUTPUT | PIA_CR_C2_HIGH | PIA_CR_C2_LOW);
    ASSERT_TRUE(pia.ca2);            /* bit3=1 → CA2 high */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_C2_OUTPUT | PIA_CR_C2_HIGH);
    ASSERT_FALSE(pia.ca2);           /* bit3=0 → CA2 low */
    ASSERT_TRUE(cb_ca2_n >= 2);
    /* CA2 as output must not raise IRQ2 even on (ignored) input toggles. */
    pia6821_set_ca2(&pia, true);
    ASSERT_FALSE(pia6821_read(&pia, PIA_RS_CRA) & PIA_CR_IRQ2_FLAG);
}

TEST(test_ca2_output_handshake) {
    setup();
    /* Port A all output, CR bit2=1 to address the port. */
    pia6821_write(&pia, PIA_RS_CRA, 0);
    pia6821_write(&pia, PIA_RS_PRA, 0xFF);
    /* CA2 output handshake: bit5=1, bit4=0, bit3=0, plus DDR-select. */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_C2_OUTPUT | PIA_CR_DDR_SELECT);
    ASSERT_TRUE(pia.ca2);            /* idles high */
    pia6821_read(&pia, PIA_RS_PRA);  /* read PRA → CA2 goes low */
    ASSERT_FALSE(pia.ca2);
    pia6821_set_ca1(&pia, true);
    pia6821_set_ca1(&pia, false);    /* active CA1 edge → CA2 back high */
    ASSERT_TRUE(pia.ca2);
}

TEST(test_ca2_output_pulse) {
    setup();
    pia6821_write(&pia, PIA_RS_CRA, 0);
    pia6821_write(&pia, PIA_RS_PRA, 0xFF);
    /* Pulse mode: bit5=1, bit4=0, bit3=1. */
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_C2_OUTPUT | PIA_CR_C2_LOW | PIA_CR_DDR_SELECT);
    cb_ca2_n = 0;
    pia6821_read(&pia, PIA_RS_PRA);  /* one-cycle strobe: low then high */
    ASSERT_TRUE(pia.ca2);            /* ends high */
    ASSERT_TRUE(cb_ca2_n >= 2);      /* saw both transitions */
}

TEST(test_cb2_output_handshake_on_write) {
    setup();
    /* Port B handshake strobes on PRB *write* (not read). */
    pia6821_write(&pia, PIA_RS_CRB, 0);
    pia6821_write(&pia, PIA_RS_PRB, 0xFF);   /* DDRB all output */
    pia6821_write(&pia, PIA_RS_CRB, PIA_CR_C2_OUTPUT | PIA_CR_DDR_SELECT);
    ASSERT_TRUE(pia.cb2);
    pia6821_write(&pia, PIA_RS_PRB, 0x42);   /* write PRB → CB2 low */
    ASSERT_FALSE(pia.cb2);
    pia6821_set_cb1(&pia, true);
    pia6821_set_cb1(&pia, false);            /* active CB1 → CB2 high */
    ASSERT_TRUE(pia.cb2);
}

TEST(test_irq_callback_fires) {
    setup();
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_IRQ1_ENABLE);
    cb_irqa_n = 0;
    pia6821_set_ca1(&pia, true);
    pia6821_set_ca1(&pia, false);            /* IRQ asserted */
    ASSERT_TRUE(cb_irqa && cb_irqa_n == 1);
    pia6821_write(&pia, PIA_RS_CRA, PIA_CR_IRQ1_ENABLE | PIA_CR_DDR_SELECT);
    pia6821_read(&pia, PIA_RS_PRA);          /* clears → IRQ deasserted */
    ASSERT_TRUE(!cb_irqa && cb_irqa_n == 2);
}

TEST(test_side_b_independent) {
    setup();
    pia6821_write(&pia, PIA_RS_CRB, PIA_CR_IRQ1_ENABLE);
    pia6821_set_cb1(&pia, true);
    pia6821_set_cb1(&pia, false);
    ASSERT_TRUE(pia6821_irqb(&pia));
    ASSERT_FALSE(pia6821_irqa(&pia));        /* side A untouched */
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Motorola MC6821 PIA Tests\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    RUN(test_reset_state);
    RUN(test_ddr_or_select);
    RUN(test_input_read_mix);
    RUN(test_port_a_callback);
    RUN(test_ca1_irq_falling);
    RUN(test_ca1_irq_rising_edge_select);
    RUN(test_ca1_flag_without_enable);
    RUN(test_ca2_input_irq);
    RUN(test_ca2_output_set_reset);
    RUN(test_ca2_output_handshake);
    RUN(test_ca2_output_pulse);
    RUN(test_cb2_output_handshake_on_write);
    RUN(test_irq_callback_fires);
    RUN(test_side_b_independent);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
