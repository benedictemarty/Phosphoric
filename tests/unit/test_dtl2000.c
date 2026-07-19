/**
 * @file test_dtl2000.c
 * @brief Digitelec DTL 2000 (PIA 6821 + ACIA 6850) unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 * @version 1.0.0
 *
 * Verifies the faithful DTL 2000 modem-card model bit-for-bit against the
 * register values extracted (by OCR) from the period programming manual
 * "Programmation carte DTL V23": PIA Port A line/mode bits, ACIA 6850
 * control/status semantics, asymmetric/symmetric init sequences, and a
 * loopback round-trip through the serial backend.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/dtl2000.h"
#include "io/serial_backend.h"

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

/* ═══════════════════════════════════════════════════════════════════════
 *  Setup helpers
 * ═══════════════════════════════════════════════════════════════════════ */

#define BASE  DTL2000_DEFAULT_BASE
#define A_PA  (BASE + DTL_REG_PIA_A)
#define A_CRA (BASE + DTL_REG_PIA_CRA)
#define A_PB  (BASE + DTL_REG_PIA_B)
#define A_CRB (BASE + DTL_REG_PIA_CRB)
#define A_CS  (BASE + DTL_REG_ACIA_CS)
#define A_DAT (BASE + DTL_REG_ACIA_D)

static dtl2000_t dev;
static serial_backend_t* loopback;

static void setup(void) {
    dtl2000_init(&dev, DTL2000_DEFAULT_BASE);
    loopback = serial_backend_loopback_create();
    loopback->open(loopback);
    dtl2000_set_backend(&dev, loopback);
}

static void teardown(void) {
    if (loopback) {
        serial_backend_destroy(loopback);
        loopback = NULL;
    }
}

/* Run the asymmetric V23 init sequence from the manual (transposed Oric). */
static void init_asymmetric(void) {
    dtl2000_write(&dev, A_CRA, 0x00);   /* CRA: select DDRA */
    dtl2000_write(&dev, A_PA, DTL_DDRA_INIT);   /* DDRA = $F4 */
    dtl2000_write(&dev, A_CS, DTL_ACIA_RESET);  /* ACIA master reset $03 */
    dtl2000_write(&dev, A_CRA, 0x04);   /* CRA: select OR (CR2=1) */
    dtl2000_write(&dev, A_PA, DTL_OR_ASYM_DISCONNECT); /* OR=$D4, line open */
    dtl2000_write(&dev, A_CS, DTL_ACIA_ASYM_CFG);      /* control=$49 (7E1) */
}

/* Tick enough cycles to cross one RX byte period at the given baud. */
static void tick_one_byte(int baud) {
    int total = (1000000 / baud) * 12 + 16;
    while (total > 0) {
        int step = (total >= 4) ? 4 : total;
        dtl2000_tick(&dev, step);
        total -= step;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(test_init_state) {
    dtl2000_t d;
    dtl2000_init(&d, DTL2000_DEFAULT_BASE);

    /* Power-on: transmitter empty; line open → carrier absent (DCD=1),
     * not clear to send (CTS=1). */
    uint8_t st = dtl2000_read(&d, A_CS);
    ASSERT_TRUE(st & DTL_ACIA_SR_TDRE);
    ASSERT_FALSE(st & DTL_ACIA_SR_RDRF);
    ASSERT_TRUE(st & DTL_ACIA_SR_DCD);   /* carrier lost */
    ASSERT_TRUE(st & DTL_ACIA_SR_CTS);   /* not clear */
    ASSERT_FALSE(d.line_connected);
}

TEST(test_addr_in_range) {
    dtl2000_t d;
    dtl2000_init(&d, DTL2000_DEFAULT_BASE);
    ASSERT_FALSE(dtl2000_addr_in_range(&d, 0x03F7));
    ASSERT_TRUE(dtl2000_addr_in_range(&d, 0x03F8));
    ASSERT_TRUE(dtl2000_addr_in_range(&d, 0x03FD));
    ASSERT_FALSE(dtl2000_addr_in_range(&d, 0x03FE));
    ASSERT_FALSE(dtl2000_addr_in_range(&d, 0x0300));
}

TEST(test_pia_ddr_or_select) {
    dtl2000_t d;
    dtl2000_init(&d, DTL2000_DEFAULT_BASE);

    /* CR2=0 → Port A address routes to DDRA */
    dtl2000_write(&d, A_CRA, 0x00);
    dtl2000_write(&d, A_PA, 0xF4);
    ASSERT_EQ(dtl2000_read(&d, A_PA), 0xF4);   /* reads DDRA */

    /* CR2=1 → Port A address routes to the output register */
    dtl2000_write(&d, A_CRA, 0x04);
    dtl2000_write(&d, A_PA, 0xD4);
    ASSERT_EQ(dtl2000_read(&d, A_PA), 0xD4);   /* reads ORA */
    /* DDRA preserved underneath */
    dtl2000_write(&d, A_CRA, 0x00);
    ASSERT_EQ(dtl2000_read(&d, A_PA), 0xF4);
}

TEST(test_line_connect_disconnect) {
    setup();
    init_asymmetric();   /* leaves line open ($D4) */

    /* Line open → not connected, carrier absent */
    uint8_t st = dtl2000_read(&dev, A_CS);
    ASSERT_FALSE(dev.line_connected);
    ASSERT_TRUE(st & DTL_ACIA_SR_DCD);

    /* Connect: OR bit2 → 0 ($D0). Backend (loopback) is connected, so the
     * carrier becomes present → DCD bit clears. */
    dtl2000_write(&dev, A_PA, DTL_OR_ASYM_CONNECT);  /* $D0 */
    ASSERT_TRUE(dev.line_connected);
    st = dtl2000_read(&dev, A_CS);
    ASSERT_FALSE(st & DTL_ACIA_SR_DCD);   /* carrier present */
    ASSERT_FALSE(st & DTL_ACIA_SR_CTS);   /* clear to send */

    /* Disconnect again: OR bit2 → 1 ($D4) */
    dtl2000_write(&dev, A_PA, DTL_OR_ASYM_DISCONNECT);
    ASSERT_FALSE(dev.line_connected);
    st = dtl2000_read(&dev, A_CS);
    ASSERT_TRUE(st & DTL_ACIA_SR_DCD);

    teardown();
}

TEST(test_mode_select_bit4) {
    setup();
    dtl2000_write(&dev, A_CRA, 0x04);   /* OR select */

    /* Asymmetric: OR bit4 = 1 ($D0/$D4) */
    dtl2000_write(&dev, A_PA, DTL_OR_ASYM_CONNECT);  /* $D0 */
    ASSERT_FALSE(dev.symmetric);
    ASSERT_EQ(dev.tx_baud, DTL_V23_TX_BAUD);   /* 75 */
    ASSERT_EQ(dev.rx_baud, DTL_V23_RX_BAUD);   /* 1200 */

    /* Symmetric: OR bit4 = 0 ($C0/$C4) */
    dtl2000_write(&dev, A_PA, DTL_OR_SYM_CONNECT);   /* $C0 */
    ASSERT_TRUE(dev.symmetric);
    ASSERT_EQ(dev.tx_baud, 1200);
    ASSERT_EQ(dev.rx_baud, 1200);

    teardown();
}

TEST(test_acia_master_reset) {
    setup();
    /* Inject a received byte first */
    dev.acia.status |= DTL_ACIA_SR_RDRF;
    dtl2000_write(&dev, A_CS, DTL_ACIA_RESET);   /* $03 */
    uint8_t st = dtl2000_read(&dev, A_CS);
    ASSERT_FALSE(st & DTL_ACIA_SR_RDRF);
    ASSERT_TRUE(st & DTL_ACIA_SR_TDRE);
    teardown();
}

TEST(test_acia_word_select_mask) {
    setup();

    /* $49 = 7E1 → 7-bit data mask */
    dtl2000_write(&dev, A_CS, DTL_ACIA_ASYM_CFG);   /* $49 */
    ASSERT_EQ(dev.acia.bitmask, 0x7F);
    ASSERT_EQ(dev.acia.framebits, 10);   /* start+7+parity+1 stop */

    /* $55 = 8N1 → 8-bit data mask */
    dtl2000_write(&dev, A_CS, DTL_ACIA_SYM_CFG);    /* $55 */
    ASSERT_EQ(dev.acia.bitmask, 0xFF);
    ASSERT_EQ(dev.acia.framebits, 10);   /* start+8+0+1 stop */

    teardown();
}

TEST(test_acia_carrier_emission_bit) {
    setup();

    /* $49: TC = 10 → RTS high → NOT emitting */
    dtl2000_write(&dev, A_CS, DTL_ACIA_ASYM_CFG);   /* $49 */
    ASSERT_FALSE(dev.acia.rts_low);

    /* $09: TC = 00 → RTS low → emitting carrier */
    dtl2000_write(&dev, A_CS, DTL_ACIA_ASYM_EMIT);  /* $09 */
    ASSERT_TRUE(dev.acia.rts_low);

    /* Symmetric: $55 no emit, $15 emit */
    dtl2000_write(&dev, A_CS, DTL_ACIA_SYM_CFG);    /* $55 */
    ASSERT_FALSE(dev.acia.rts_low);
    dtl2000_write(&dev, A_CS, DTL_ACIA_SYM_EMIT);   /* $15 */
    ASSERT_TRUE(dev.acia.rts_low);

    teardown();
}

TEST(test_tx_clears_tdre) {
    setup();
    init_asymmetric();
    dtl2000_write(&dev, A_PA, DTL_OR_ASYM_CONNECT);  /* connect */
    dtl2000_write(&dev, A_CS, DTL_ACIA_ASYM_EMIT);   /* emit carrier */

    /* TDRE set before write */
    ASSERT_TRUE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_TDRE);

    dtl2000_write(&dev, A_DAT, 'Z');
    /* Transmitter busy → TDRE clears */
    ASSERT_FALSE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_TDRE);

    /* After one TX byte period at 75 baud, TDRE returns */
    tick_one_byte(75);
    ASSERT_TRUE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_TDRE);

    teardown();
}

TEST(test_loopback_roundtrip) {
    setup();
    /* Connect line, symmetric 8-bit so the byte survives unmasked */
    dtl2000_write(&dev, A_CRA, 0x04);
    dtl2000_write(&dev, A_PA, DTL_OR_SYM_CONNECT);   /* $C0 connect, symmetric */
    dtl2000_write(&dev, A_CS, DTL_ACIA_RESET);       /* master reset */
    dtl2000_write(&dev, A_CS, DTL_ACIA_SYM_EMIT);    /* $15 8N1 emit */

    /* Transmit a byte; loopback echoes it back */
    dtl2000_write(&dev, A_DAT, 0x42);

    /* Initially no RX */
    ASSERT_FALSE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_RDRF);

    /* Tick across an RX byte period at 1200 baud */
    tick_one_byte(1200);

    uint8_t st = dtl2000_read(&dev, A_CS);
    ASSERT_TRUE(st & DTL_ACIA_SR_RDRF);
    ASSERT_EQ(dtl2000_read(&dev, A_DAT), 0x42);

    /* Reading data clears RDRF */
    ASSERT_FALSE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_RDRF);

    teardown();
}

TEST(test_no_tx_when_line_open) {
    setup();
    /* Configure to emit but leave line OPEN (not connected) */
    dtl2000_write(&dev, A_CRA, 0x04);
    dtl2000_write(&dev, A_PA, DTL_OR_SYM_DISCONNECT); /* $C4 open */
    dtl2000_write(&dev, A_CS, DTL_ACIA_RESET);
    dtl2000_write(&dev, A_CS, DTL_ACIA_SYM_EMIT);

    dtl2000_write(&dev, A_DAT, 0x55);
    tick_one_byte(1200);
    /* Nothing should have been transmitted/echoed because the line is open */
    ASSERT_FALSE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_RDRF);
    ASSERT_EQ(dev.tx_count, 0u);

    teardown();
}

TEST(test_reset_clears_state) {
    setup();
    init_asymmetric();
    dtl2000_write(&dev, A_PA, DTL_OR_ASYM_CONNECT);
    ASSERT_TRUE(dev.line_connected);

    dtl2000_reset(&dev);
    ASSERT_FALSE(dev.line_connected);
    ASSERT_FALSE(dev.symmetric);
    ASSERT_EQ(dev.tx_count, 0u);
    ASSERT_EQ(dev.rx_count, 0u);
    uint8_t st = dtl2000_read(&dev, A_CS);
    ASSERT_TRUE(st & DTL_ACIA_SR_TDRE);
    ASSERT_TRUE(st & DTL_ACIA_SR_DCD);

    teardown();
}

/* ───────────────────────────────────────────────────────────────────────
 *  IRQ probe — records the level-triggered IRQ line state via the hooks
 * ─────────────────────────────────────────────────────────────────────── */

static int probe_irq_line = 0;   /* 1 = asserted */
static void probe_irq_set(emulator_t* e) { (void)e; probe_irq_line = 1; }
static void probe_irq_clr(emulator_t* e) { (void)e; probe_irq_line = 0; }

static void attach_irq_probe(void) {
    probe_irq_line = 0;
    dev.irq_set = probe_irq_set;
    dev.irq_clr = probe_irq_clr;
    dev.irq_userdata = NULL;
}

TEST(test_irq_disabled_by_default) {
    setup();
    attach_irq_probe();
    /* No RIE / no TIE: configure 8N1 emit, connect, send/receive a byte */
    dtl2000_write(&dev, A_CRA, 0x04);
    dtl2000_write(&dev, A_PA, DTL_OR_SYM_CONNECT);
    dtl2000_write(&dev, A_CS, DTL_ACIA_RESET);
    dtl2000_write(&dev, A_CS, DTL_ACIA_SYM_EMIT);   /* $15: RIE=0, TC=00 */
    dtl2000_write(&dev, A_DAT, 0x33);
    tick_one_byte(1200);
    /* RDRF set but interrupts disabled → no IRQ */
    ASSERT_TRUE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_RDRF);
    ASSERT_FALSE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_IRQ);
    ASSERT_EQ(probe_irq_line, 0);
    teardown();
}

TEST(test_irq_on_rx_with_rie) {
    setup();
    attach_irq_probe();
    dtl2000_write(&dev, A_CRA, 0x04);
    dtl2000_write(&dev, A_PA, DTL_OR_SYM_CONNECT);
    dtl2000_write(&dev, A_CS, DTL_ACIA_RESET);
    /* $15 (8N1 emit) + RIE($80) = $95 */
    dtl2000_write(&dev, A_CS, (uint8_t)(DTL_ACIA_SYM_EMIT | DTL_ACIA_CR_RIE));
    ASSERT_EQ(probe_irq_line, 0);

    /* Transmit a byte; loopback echoes; RX poll sets RDRF → IRQ asserts */
    dtl2000_write(&dev, A_DAT, 0x44);
    tick_one_byte(1200);
    uint8_t st = dtl2000_read(&dev, A_CS);
    ASSERT_TRUE(st & DTL_ACIA_SR_RDRF);
    ASSERT_TRUE(st & DTL_ACIA_SR_IRQ);
    ASSERT_EQ(probe_irq_line, 1);

    /* Reading the data clears RDRF → IRQ deasserts */
    (void)dtl2000_read(&dev, A_DAT);
    ASSERT_FALSE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_IRQ);
    ASSERT_EQ(probe_irq_line, 0);
    teardown();
}

TEST(test_irq_on_tx_with_tie) {
    setup();
    attach_irq_probe();
    dtl2000_write(&dev, A_CRA, 0x04);
    dtl2000_write(&dev, A_PA, DTL_OR_SYM_CONNECT);
    dtl2000_write(&dev, A_CS, DTL_ACIA_RESET);
    /* 8N1 with TC=01 (RTS low + TIE on): WS=101<<2=$14, CDS=01, TC=01<<5=$20 → $35 */
    dtl2000_write(&dev, A_CS, 0x35);
    /* TDRE is set after reset → with TIE, IRQ asserts immediately */
    ASSERT_TRUE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_TDRE);
    ASSERT_EQ(probe_irq_line, 1);

    /* Writing data clears TDRE → IRQ deasserts while shifting out */
    dtl2000_write(&dev, A_DAT, 0x55);
    ASSERT_EQ(probe_irq_line, 0);

    /* After the byte shifts out, TDRE returns → IRQ re-asserts */
    tick_one_byte(1200);
    ASSERT_TRUE(dtl2000_read(&dev, A_CS) & DTL_ACIA_SR_TDRE);
    ASSERT_EQ(probe_irq_line, 1);
    teardown();
}

/* Savestate (Epic 7 / US4) : l'état émulé est restauré, les pointeurs hôte
 * (backend/callbacks) sont PRÉSERVÉS de l'instance cible (jamais écrasés par les
 * valeurs périmées du blob). */
TEST(test_savestate_roundtrip_preserves_host_pointers) {
    setup();
    /* État émulé distinctif (posé directement : ce test cible la sérialisation,
     * pas le décodage des registres). */
    dev.line_connected = true;
    dev.rx_count = 7;
    dev.tx_count = 9;
    dev.acia.control = 0x35;
    ASSERT_TRUE(dev.line_connected);

    /* Sauvegarde */
    FILE* f = fopen("/tmp/dtl_savestate_test.bin", "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(dtl2000_save(&dev, f));
    fclose(f);

    /* Instance cible avec des pointeurs hôte SENTINELLES à préserver */
    dtl2000_t d2;
    memset(&d2, 0, sizeof(d2));
    d2.backend        = (serial_backend_t*)0x1234;
    d2.acia.userdata  = (void*)0x5678;
    d2.pia.userdata   = (void*)0x9ABC;

    FILE* g = fopen("/tmp/dtl_savestate_test.bin", "rb");
    ASSERT_TRUE(g != NULL);
    dtl2000_load(&d2, g, (uint32_t)sizeof(dtl2000_t));
    fclose(g);

    /* État émulé restauré */
    ASSERT_TRUE(d2.line_connected);
    ASSERT_EQ(d2.rx_count, 7);
    ASSERT_EQ(d2.tx_count, 9);
    /* Pointeurs hôte préservés (sentinelles, PAS les valeurs sauvées) */
    ASSERT_TRUE(d2.backend == (serial_backend_t*)0x1234);
    ASSERT_TRUE(d2.acia.userdata == (void*)0x5678);
    ASSERT_TRUE(d2.pia.userdata == (void*)0x9ABC);

    /* Garde par taille : une taille erronée ne touche à rien */
    dtl2000_t d3;
    memset(&d3, 0, sizeof(d3));
    d3.rx_count = 123;
    FILE* h = fopen("/tmp/dtl_savestate_test.bin", "rb");
    dtl2000_load(&d3, h, 999999);   /* mauvaise taille → ignorée */
    fclose(h);
    ASSERT_EQ(d3.rx_count, 123);

    remove("/tmp/dtl_savestate_test.bin");
    teardown();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n=== Digitelec DTL 2000 (PIA 6821 + ACIA 6850) Tests ===\n\n");

    RUN(test_init_state);
    RUN(test_addr_in_range);
    RUN(test_pia_ddr_or_select);
    RUN(test_line_connect_disconnect);
    RUN(test_mode_select_bit4);
    RUN(test_acia_master_reset);
    RUN(test_acia_word_select_mask);
    RUN(test_acia_carrier_emission_bit);
    RUN(test_tx_clears_tdre);
    RUN(test_loopback_roundtrip);
    RUN(test_no_tx_when_line_open);
    RUN(test_reset_clears_state);
    RUN(test_irq_disabled_by_default);
    RUN(test_irq_on_rx_with_rie);
    RUN(test_irq_on_tx_with_tie);
    RUN(test_savestate_roundtrip_preserves_host_pointers);

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
