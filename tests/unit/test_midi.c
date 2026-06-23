/**
 * @file test_midi.c
 * @brief Mageco MIDI interface (MC6850 ACIA at $03FE) unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-23
 * @version 1.0.0
 *
 * Verifies the Mageco MIDI card model (forum t=2525): the two-register MC6850
 * window at $03FE-$03FF, the fixed 31250-baud MIDI cadence, the "no handshake"
 * line semantics (DCD/CTS pinned present/clear), a loopback round-trip, file
 * capture/replay of the raw MIDI byte stream, and the IRQ output.
 *
 * The file-capture tests double as proof of the emulator↔hardware equivalence:
 * the bytes captured here are exactly what a real Oric+Mageco card would put on
 * the MIDI OUT current loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/mageco.h"
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

#define BASE  MAGECO_DEFAULT_BASE
#define A_CS  (BASE + MAGECO_REG_ACIA_CS)   /* $03FE control/status */
#define A_DAT (BASE + MAGECO_REG_ACIA_D)    /* $03FF data           */

/* Standard 8-N-1 control word: word-select=8N1 (bits2-4 = 101 → $14),
 * divide ÷16 (bits0-1 = 01 → $01), RTS low / Tx IRQ off (bits5-6 = 00). */
#define MIDI_8N1_CFG   0x15

static mageco_t dev;
static serial_backend_t* backend;

static void setup_loopback(void) {
    mageco_init(&dev, MAGECO_DEFAULT_BASE);
    backend = serial_backend_loopback_create();
    backend->open(backend);
    mageco_set_backend(&dev, backend);
    mageco_write(&dev, A_CS, MIDI_8N1_CFG);   /* configure 8N1 */
}

static void teardown(void) {
    if (backend) {
        serial_backend_destroy(backend);
        backend = NULL;
    }
}

/* Tick across one full MIDI byte period (31250 baud, 10 bits ≈ 320 cycles). */
static void tick_one_byte(void) {
    int total = 400;
    while (total > 0) {
        int step = (total >= 4) ? 4 : total;
        mageco_tick(&dev, step);
        total -= step;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  IRQ probe
 * ═══════════════════════════════════════════════════════════════════════ */

static int probe_irq_line = 0;
static void probe_irq_set(emulator_t* e) { (void)e; probe_irq_line = 1; }
static void probe_irq_clr(emulator_t* e) { (void)e; probe_irq_line = 0; }

static void attach_irq_probe(void) {
    probe_irq_line = 0;
    dev.irq_set = probe_irq_set;
    dev.irq_clr = probe_irq_clr;
    dev.irq_userdata = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(test_init_state) {
    mageco_t d;
    mageco_init(&d, MAGECO_DEFAULT_BASE);

    /* Power-on: transmitter empty, no byte received. */
    uint8_t st = mageco_read(&d, A_CS);
    ASSERT_TRUE(st & ACIA6850_SR_TDRE);
    ASSERT_FALSE(st & ACIA6850_SR_RDRF);
    /* MIDI has no modem handshake: DCD/CTS pinned "present/clear" → both 0. */
    ASSERT_FALSE(st & ACIA6850_SR_DCD);
    ASSERT_FALSE(st & ACIA6850_SR_CTS);
}

TEST(test_addr_in_range) {
    mageco_t d;
    mageco_init(&d, MAGECO_DEFAULT_BASE);
    ASSERT_FALSE(mageco_addr_in_range(&d, 0x03FD));
    ASSERT_TRUE(mageco_addr_in_range(&d, 0x03FE));
    ASSERT_TRUE(mageco_addr_in_range(&d, 0x03FF));
    ASSERT_FALSE(mageco_addr_in_range(&d, 0x0400));
    ASSERT_FALSE(mageco_addr_in_range(&d, 0x0300));
}

TEST(test_default_base_is_03fe) {
    ASSERT_EQ(MAGECO_DEFAULT_BASE, 0x03FE);
    ASSERT_EQ(MAGECO_REG_SPAN, 2);
}

TEST(test_midi_baud_timing) {
    mageco_t d;
    mageco_init(&d, MAGECO_DEFAULT_BASE);
    mageco_write(&d, A_CS, MIDI_8N1_CFG);   /* 8N1 → 10 bits/frame */

    /* 1 MHz * 10 bits / 31250 baud = 320 cycles per byte. */
    ASSERT_EQ(d.tx_reload, 320);
    ASSERT_EQ(d.rx_reload, 320);
}

TEST(test_master_reset) {
    setup_loopback();
    /* Queue a byte (clears TDRE), then master-reset and check TDRE restored. */
    mageco_write(&dev, A_DAT, 0x90);
    ASSERT_FALSE(mageco_read(&dev, A_CS) & ACIA6850_SR_TDRE);
    mageco_write(&dev, A_CS, 0x03);   /* CDS=11 → master reset */
    ASSERT_TRUE(mageco_read(&dev, A_CS) & ACIA6850_SR_TDRE);
    ASSERT_FALSE(dev.tx_busy);
    teardown();
}

TEST(test_tx_clears_then_restores_tdre) {
    setup_loopback();
    ASSERT_TRUE(mageco_read(&dev, A_CS) & ACIA6850_SR_TDRE);
    mageco_write(&dev, A_DAT, 0x3C);
    /* Writing data makes the transmitter busy → TDRE clear. */
    ASSERT_FALSE(mageco_read(&dev, A_CS) & ACIA6850_SR_TDRE);
    /* After one byte period the byte has shifted out → TDRE set again. */
    tick_one_byte();
    ASSERT_TRUE(mageco_read(&dev, A_CS) & ACIA6850_SR_TDRE);
    teardown();
}

TEST(test_loopback_roundtrip) {
    setup_loopback();
    /* Transmit a byte; the loopback echoes it back onto MIDI IN. */
    mageco_write(&dev, A_DAT, 0x42);
    ASSERT_FALSE(mageco_read(&dev, A_CS) & ACIA6850_SR_RDRF);

    tick_one_byte();
    uint8_t st = mageco_read(&dev, A_CS);
    ASSERT_TRUE(st & ACIA6850_SR_RDRF);
    ASSERT_EQ(mageco_read(&dev, A_DAT), 0x42);
    /* Reading data clears RDRF. */
    ASSERT_FALSE(mageco_read(&dev, A_CS) & ACIA6850_SR_RDRF);
    teardown();
}

TEST(test_tx_always_sends_no_handshake) {
    /* Unlike the DTL 2000, the Mageco card has no line-connect gate: a written
     * byte always reaches the backend (MIDI OUT is a direct current loop). */
    setup_loopback();
    ASSERT_EQ(dev.tx_count, 0u);
    mageco_write(&dev, A_DAT, 0x7F);
    ASSERT_EQ(dev.tx_count, 1u);
    teardown();
}

/* Capture a 3-byte MIDI "Note On" to a file and verify the raw bytes — this is
 * exactly what a real Oric+Mageco card emits on the MIDI OUT DIN socket. */
TEST(test_file_capture_note_on) {
    const char* path = "test_midi_capture.bin";
    remove(path);

    mageco_init(&dev, MAGECO_DEFAULT_BASE);
    backend = serial_backend_file_create(NULL, path);
    ASSERT_TRUE(backend != NULL);
    ASSERT_TRUE(backend->open(backend));
    mageco_set_backend(&dev, backend);
    mageco_write(&dev, A_CS, MIDI_8N1_CFG);

    /* Note On, channel 1, middle C, velocity 127. */
    mageco_write(&dev, A_DAT, 0x90); tick_one_byte();
    mageco_write(&dev, A_DAT, 0x3C); tick_one_byte();
    mageco_write(&dev, A_DAT, 0x7F); tick_one_byte();

    serial_backend_destroy(backend);   /* flush + close the capture file */
    backend = NULL;

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    uint8_t buf[8] = {0};
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    remove(path);

    ASSERT_EQ((int)n, 3);
    ASSERT_EQ(buf[0], 0x90);
    ASSERT_EQ(buf[1], 0x3C);
    ASSERT_EQ(buf[2], 0x7F);
}

/* Replay a MIDI byte stream from a file into MIDI IN. */
TEST(test_file_replay_in) {
    const char* path = "test_midi_replay.bin";
    FILE* f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fputc(0xB0, f);   /* Control Change */
    fputc(0x07, f);   /* main volume */
    fputc(0x64, f);
    fclose(f);

    mageco_init(&dev, MAGECO_DEFAULT_BASE);
    backend = serial_backend_file_create(path, NULL);
    ASSERT_TRUE(backend != NULL);
    ASSERT_TRUE(backend->open(backend));
    mageco_set_backend(&dev, backend);
    mageco_write(&dev, A_CS, MIDI_8N1_CFG);

    uint8_t expect[3] = {0xB0, 0x07, 0x64};
    for (int i = 0; i < 3; i++) {
        tick_one_byte();
        ASSERT_TRUE(mageco_read(&dev, A_CS) & ACIA6850_SR_RDRF);
        ASSERT_EQ(mageco_read(&dev, A_DAT), expect[i]);
    }

    serial_backend_destroy(backend);
    backend = NULL;
    remove(path);
}

TEST(test_irq_disabled_by_default) {
    setup_loopback();
    attach_irq_probe();
    /* No RIE, no TIE → no IRQ even after a round-trip. */
    mageco_write(&dev, A_DAT, 0x50);
    tick_one_byte();
    ASSERT_TRUE(mageco_read(&dev, A_CS) & ACIA6850_SR_RDRF);
    ASSERT_FALSE(mageco_read(&dev, A_CS) & ACIA6850_SR_IRQ);
    ASSERT_EQ(probe_irq_line, 0);
    teardown();
}

TEST(test_irq_on_rx_with_rie) {
    setup_loopback();
    attach_irq_probe();
    /* 8N1 emit + RIE($80). */
    mageco_write(&dev, A_CS, (uint8_t)(MIDI_8N1_CFG | ACIA6850_CR_RIE));
    ASSERT_EQ(probe_irq_line, 0);

    mageco_write(&dev, A_DAT, 0x44);
    tick_one_byte();
    uint8_t st = mageco_read(&dev, A_CS);
    ASSERT_TRUE(st & ACIA6850_SR_RDRF);
    ASSERT_TRUE(st & ACIA6850_SR_IRQ);
    ASSERT_EQ(probe_irq_line, 1);

    /* Reading the data clears RDRF → IRQ deasserts. */
    (void)mageco_read(&dev, A_DAT);
    ASSERT_FALSE(mageco_read(&dev, A_CS) & ACIA6850_SR_IRQ);
    ASSERT_EQ(probe_irq_line, 0);
    teardown();
}

/* ── ORICON variant (6850 at $031C/$031D + clock generator $031E/$031F) ── */

#define O_BASE  MAGECO_ORICON_BASE
#define O_CS    (O_BASE + MAGECO_REG_ACIA_CS)    /* $031C */
#define O_DAT   (O_BASE + MAGECO_REG_ACIA_D)     /* $031D */
#define O_CLKLO (O_BASE + MAGECO_REG_CLKGEN_LO)  /* $031E */
#define O_CLKHI (O_BASE + MAGECO_REG_CLKGEN_HI)  /* $031F */

TEST(test_oricon_init_and_range) {
    mageco_t d;
    mageco_init_oricon(&d, MAGECO_ORICON_BASE);
    ASSERT_EQ(d.base_addr, 0x031C);
    ASSERT_EQ(d.span, 4);
    ASSERT_TRUE(d.oricon);
    /* 4-byte window $031C-$031F */
    ASSERT_FALSE(mageco_addr_in_range(&d, 0x031B));
    ASSERT_TRUE(mageco_addr_in_range(&d, 0x031C));
    ASSERT_TRUE(mageco_addr_in_range(&d, 0x031F));
    ASSERT_FALSE(mageco_addr_in_range(&d, 0x0320));
}

TEST(test_oricon_clock_generator_latches) {
    mageco_t d;
    mageco_init_oricon(&d, MAGECO_ORICON_BASE);
    /* $031E/$031F are readable/writable latches in ORICON mode */
    mageco_write(&d, O_CLKLO, 0x10);
    mageco_write(&d, O_CLKHI, 0x27);
    ASSERT_EQ(mageco_read(&d, O_CLKLO), 0x10);
    ASSERT_EQ(mageco_read(&d, O_CLKHI), 0x27);
}

TEST(test_mageco_mode_has_no_clock_registers) {
    mageco_t d;
    mageco_init(&d, MAGECO_DEFAULT_BASE);   /* original Mageco: 2-register window */
    ASSERT_EQ(d.span, 2);
    ASSERT_FALSE(d.oricon);
    /* $03FE+2/$03FE+3 are outside the 2-byte window. */
    ASSERT_FALSE(mageco_addr_in_range(&d, 0x0400));
    ASSERT_TRUE(mageco_addr_in_range(&d, 0x03FF));
}

TEST(test_oricon_acia_roundtrip) {
    mageco_t d;
    mageco_init_oricon(&d, MAGECO_ORICON_BASE);
    serial_backend_t* lb = serial_backend_loopback_create();
    lb->open(lb);
    mageco_set_backend(&d, lb);
    mageco_write(&d, O_CS, MIDI_8N1_CFG);

    /* The 6850 works identically at the ORICON base. */
    mageco_write(&d, O_DAT, 0x55);
    int total = 400;
    while (total > 0) { int s = total >= 4 ? 4 : total; mageco_tick(&d, s); total -= s; }
    ASSERT_TRUE(mageco_read(&d, O_CS) & ACIA6850_SR_RDRF);
    ASSERT_EQ(mageco_read(&d, O_DAT), 0x55);

    serial_backend_destroy(lb);
}

/* The ALSA real-time MIDI backend is an optional build feature (MIDI=1). In a
 * default build it must degrade gracefully: the factory returns NULL with a log
 * line rather than crashing, so `--mageco midi` reports a clear "rebuild" error. */
#ifndef HAS_MIDI
TEST(test_midi_alsa_stub_without_build) {
    serial_backend_t* b = serial_backend_midi_create("128:0");
    ASSERT_TRUE(b == NULL);
}
#endif

TEST(test_irq_on_tx_with_tie) {
    setup_loopback();
    attach_irq_probe();
    /* Transmit-control = 01 (RTS low, Tx IRQ on) → bits6-5 = 01 → $20.
     * With 8N1 word-select ($14) + ÷16 ($01) that is $35. */
    mageco_write(&dev, A_CS, 0x35);
    /* TDRE is set at idle and Tx IRQ is enabled → IRQ asserted. */
    ASSERT_TRUE(mageco_read(&dev, A_CS) & ACIA6850_SR_IRQ);
    ASSERT_EQ(probe_irq_line, 1);
    teardown();
}

int main(void) {
    printf("\n=== Mageco MIDI interface (MC6850 @ $03FE) Tests ===\n\n");

    RUN(test_init_state);
    RUN(test_addr_in_range);
    RUN(test_default_base_is_03fe);
    RUN(test_midi_baud_timing);
    RUN(test_master_reset);
    RUN(test_tx_clears_then_restores_tdre);
    RUN(test_loopback_roundtrip);
    RUN(test_tx_always_sends_no_handshake);
    RUN(test_file_capture_note_on);
    RUN(test_file_replay_in);
    RUN(test_irq_disabled_by_default);
    RUN(test_irq_on_rx_with_rie);
    RUN(test_irq_on_tx_with_tie);
    RUN(test_oricon_init_and_range);
    RUN(test_oricon_clock_generator_latches);
    RUN(test_mageco_mode_has_no_clock_registers);
    RUN(test_oricon_acia_roundtrip);
#ifndef HAS_MIDI
    RUN(test_midi_alsa_stub_without_build);
#endif

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
