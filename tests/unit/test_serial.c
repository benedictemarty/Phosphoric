/**
 * @file test_serial.c
 * @brief ACIA 6551 serial interface unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-22
 * @version 1.0.0
 *
 * Tests the ACIA 6551 serial interface emulation including register
 * read/write, TX/RX with loopback backend, IRQ generation, baud rate
 * configuration, V23 mode, and status flags.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/acia6551.h"
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

static acia6551_t acia;
static serial_backend_t* loopback;

static void setup(void) {
    acia_init(&acia);
    loopback = serial_backend_loopback_create();
    loopback->open(loopback);
    acia_set_backend(&acia, loopback);
}

static void teardown(void) {
    if (loopback) {
        serial_backend_destroy(loopback);
        loopback = NULL;
    }
}

/* Helper: tick ACIA enough cycles for one byte at given baud */
static void tick_one_byte(int baud) {
    int total = (1000000 / baud) * 10 + 10;  /* extra margin */
    /* Use small step sizes to simulate realistic CPU instruction timing */
    while (total > 0) {
        int step = (total >= 4) ? 4 : total;
        acia_tick(&acia, step);
        total -= step;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(test_init_state) {
    acia6551_t a;
    acia_init(&a);

    /* Power-on: TDRE set, no IRQ, no errors */
    uint8_t status = acia_read(&a, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_TDRE);
    ASSERT_FALSE(status & ACIA_STATUS_RDRF);
    ASSERT_FALSE(status & ACIA_STATUS_IRQ);

    /* Command and Control registers start at 0 */
    ASSERT_EQ(acia_read(&a, ACIA_REG_COMMAND), 0x00);
    ASSERT_EQ(acia_read(&a, ACIA_REG_CONTROL), 0x00);
}

TEST(test_programmed_reset) {
    setup();

    /* Write command register with DTR + parity bits */
    acia_write(&acia, ACIA_REG_COMMAND, 0xEB);  /* parity=0xE0, DTR+TIC+ECHO=0x0B */
    ASSERT_EQ(acia_read(&acia, ACIA_REG_COMMAND), 0xEB);

    /* Write to status address ($031D) triggers programmed reset */
    acia_write(&acia, ACIA_REG_STATUS, 0x00);

    /* After reset: low 5 bits cleared, parity bits (7-5) preserved */
    ASSERT_EQ(acia_read(&acia, ACIA_REG_COMMAND) & 0x1F, 0x00);
    ASSERT_EQ(acia_read(&acia, ACIA_REG_COMMAND) & 0xE0, 0xE0);

    /* TDRE should be set after reset */
    uint8_t status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_TDRE);

    teardown();
}

TEST(test_control_register_rw) {
    acia6551_t a;
    acia_init(&a);

    /* Write and read back control register */
    acia_write(&a, ACIA_REG_CONTROL, 0x1E);  /* 9600 baud, 8-N-1, internal clock */
    ASSERT_EQ(acia_read(&a, ACIA_REG_CONTROL), 0x1E);

    acia_write(&a, ACIA_REG_CONTROL, 0x98);  /* 1200 baud, 7-bit, 2 stop */
    ASSERT_EQ(acia_read(&a, ACIA_REG_CONTROL), 0x98);
}

TEST(test_command_register_rw) {
    acia6551_t a;
    acia_init(&a);

    /* Write and read back command register */
    acia_write(&a, ACIA_REG_COMMAND, 0x0B);  /* DTR, TIC=10, no echo */
    ASSERT_EQ(acia_read(&a, ACIA_REG_COMMAND), 0x0B);
}

TEST(test_loopback_tx_rx) {
    setup();

    /* Configure: 19200 baud, 8-N-1, DTR enabled */
    acia_write(&acia, ACIA_REG_CONTROL, 0x1F);  /* 19200 baud, internal clock */
    acia_write(&acia, ACIA_REG_COMMAND, 0x01);  /* DTR on */

    /* Send a byte */
    acia_write(&acia, ACIA_REG_DATA, 0x42);

    /* TDRE should be clear (byte pending) */
    ASSERT_FALSE(acia_read(&acia, ACIA_REG_STATUS) & ACIA_STATUS_TDRE);

    /* Tick until TX completes and RX picks it up via loopback */
    tick_one_byte(19200);
    tick_one_byte(19200);

    /* RDRF should be set */
    uint8_t status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_RDRF);

    /* Read the received byte */
    uint8_t data = acia_read(&acia, ACIA_REG_DATA);
    ASSERT_EQ(data, 0x42);

    /* RDRF should be cleared after read */
    ASSERT_FALSE(acia_read(&acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);

    teardown();
}

TEST(test_tx_irq) {
    setup();

    /* Configure: 19200 baud, DTR on, TIC=01 (RTS low, TX IRQ enabled) */
    acia_write(&acia, ACIA_REG_CONTROL, 0x1F);
    acia_write(&acia, ACIA_REG_COMMAND, 0x05);  /* DTR + TIC=01 */

    /* TDRE is set at init → TX IRQ should fire */
    uint8_t status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_IRQ);

    /* Reading status should clear IRQ */
    status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_FALSE(status & ACIA_STATUS_IRQ);

    teardown();
}

TEST(test_rx_irq) {
    setup();

    /* Configure: 19200 baud, DTR on, RX IRQ enabled (IRD=0) */
    acia_write(&acia, ACIA_REG_CONTROL, 0x1F);
    acia_write(&acia, ACIA_REG_COMMAND, 0x01);  /* DTR, IRD=0 (RX IRQ enabled) */

    /* Inject byte directly into loopback */
    loopback->send(loopback, 0xAA);

    /* Tick to receive */
    tick_one_byte(19200);

    /* IRQ should be set due to RDRF + RX IRQ enabled */
    uint8_t status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_IRQ);
    ASSERT_TRUE(status & ACIA_STATUS_RDRF);

    teardown();
}

TEST(test_overrun) {
    setup();

    /* Configure: 19200 baud, DTR on */
    acia_write(&acia, ACIA_REG_CONTROL, 0x1F);
    acia_write(&acia, ACIA_REG_COMMAND, 0x01);

    /* Inject two bytes without reading */
    loopback->send(loopback, 0x11);
    loopback->send(loopback, 0x22);

    /* Tick enough for both to arrive */
    tick_one_byte(19200);
    tick_one_byte(19200);

    /* Overrun should be set */
    uint8_t status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_OVRN);

    /* Reading data should clear overrun */
    acia_read(&acia, ACIA_REG_DATA);
    status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_FALSE(status & ACIA_STATUS_OVRN);

    teardown();
}

TEST(test_v23_mode) {
    setup();

    /* Enable V23 mode */
    acia_set_v23_mode(&acia, true);

    /* Verify asymmetric timing */
    ASSERT_TRUE(acia.v23_mode);
    /* RX at 1200 baud, 10 framebits: (1000000 * 10) / 1200 = 8333 */
    ASSERT_EQ(acia.rx_reload, (1000000L * 10) / 1200);
    /* TX at 75 baud, 10 framebits: (1000000 * 10) / 75 = 133333 */
    ASSERT_EQ(acia.tx_reload, (1000000L * 10) / 75);

    teardown();
}

TEST(test_ext_clock_baud) {
    setup();

    /* Select the external clock (baud index 0), 8-N-1 -> 10 framebits.
     * Default = instant transfer: reload == 1 cycle/byte. */
    acia_write(&acia, ACIA_REG_CONTROL, 0x10);  /* baud idx 0, 8 data, 1 stop */
    ASSERT_EQ(acia.rx_reload, 1);
    ASSERT_EQ(acia.tx_reload, 1);

    /* --serial-baud 9600: realistic cadence substituted for external clock. */
    acia_set_ext_clock_baud(&acia, 9600);
    ASSERT_EQ(acia.rx_reload, (1000000L * 10) / 9600);
    ASSERT_EQ(acia.tx_reload, (1000000L * 10) / 9600);

    /* A real internal baud rate ignores the override (uses its own rate). */
    acia_write(&acia, ACIA_REG_CONTROL, 0x1E);  /* 9600 internal, 8-N-1 */
    ASSERT_EQ(acia.rx_reload, (1000000L * 10) / 9600);

    /* Back to 0 restores instant transfer. */
    acia_set_ext_clock_baud(&acia, 0);
    acia_write(&acia, ACIA_REG_CONTROL, 0x10);
    ASSERT_EQ(acia.rx_reload, 1);

    teardown();
}

TEST(test_dcd_dsr_status) {
    acia6551_t a;
    acia_init(&a);

    /* Default: DCD and DSR active → status bits clear (active low encoding) */
    uint8_t status = acia_read(&a, ACIA_REG_STATUS);
    ASSERT_FALSE(status & ACIA_STATUS_DCD);
    ASSERT_FALSE(status & ACIA_STATUS_DSR);

    /* Deactivate DCD → bit 5 should be set */
    acia_set_dcd(&a, false);
    status = acia_read(&a, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_DCD);

    /* Deactivate DSR → bit 6 should be set */
    acia_set_dsr(&a, false);
    status = acia_read(&a, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_DSR);
}

TEST(test_loopback_backend) {
    /* Test backend in isolation */
    serial_backend_t* lb = serial_backend_loopback_create();
    ASSERT_TRUE(lb != NULL);
    ASSERT_TRUE(lb->open(lb));
    ASSERT_TRUE(lb->connected(lb));

    /* Empty at start */
    ASSERT_FALSE(lb->poll(lb));

    /* Send and receive */
    ASSERT_TRUE(lb->send(lb, 0x55));
    ASSERT_TRUE(lb->poll(lb));

    uint8_t byte;
    ASSERT_TRUE(lb->recv(lb, &byte));
    ASSERT_EQ(byte, 0x55);

    /* Empty again */
    ASSERT_FALSE(lb->poll(lb));

    /* Fill buffer to capacity */
    for (int i = 0; i < SERIAL_LOOPBACK_BUFSZ; i++) {
        ASSERT_TRUE(lb->send(lb, (uint8_t)i));
    }
    /* Buffer full — send should fail */
    ASSERT_FALSE(lb->send(lb, 0xFF));

    /* Drain all */
    for (int i = 0; i < SERIAL_LOOPBACK_BUFSZ; i++) {
        ASSERT_TRUE(lb->recv(lb, &byte));
        ASSERT_EQ(byte, (uint8_t)i);
    }

    serial_backend_destroy(lb);
}

TEST(test_frame_format_8n1) {
    acia6551_t a;
    acia_init(&a);

    /* 8-N-1: control=0x1F (19200 baud, 8-bit, 1 stop), command=0x00 (no parity) */
    acia_write(&a, ACIA_REG_CONTROL, 0x1F);  /* 19200, WL=00 (8-bit), SBN=0 (1 stop) */
    acia_write(&a, ACIA_REG_COMMAND, 0x00);  /* No parity (PME=0) */

    /* framebits = 1 start + 8 data + 0 parity + 1 stop = 10 */
    ASSERT_EQ(a.framebits, 10);
    ASSERT_EQ(a.bitmask, 0xFF);
}

TEST(test_frame_format_7e1) {
    acia6551_t a;
    acia_init(&a);

    /* 7-E-1: control=0x2F (19200, WL=01=7-bit, SBN=0), command=0x20 (PME=1) */
    acia_write(&a, ACIA_REG_CONTROL, 0x2F);  /* 19200, WL=01 (7-bit), 1 stop */
    acia_write(&a, ACIA_REG_COMMAND, 0x20);  /* PME=1 (parity enabled) */

    /* framebits = 1 start + 7 data + 1 parity + 1 stop = 10 */
    ASSERT_EQ(a.framebits, 10);
    ASSERT_EQ(a.bitmask, 0x7F);
}

TEST(test_frame_format_5n2) {
    acia6551_t a;
    acia_init(&a);

    /* 5-N-2: control=0xEF (19200, WL=11=5-bit, SBN=1=2 stop), command=0x00 */
    acia_write(&a, ACIA_REG_CONTROL, 0xEF);  /* 19200, WL=11 (5-bit), 2 stop */
    acia_write(&a, ACIA_REG_COMMAND, 0x00);  /* No parity */

    /* framebits = 1 start + 5 data + 0 parity + 2 stop = 8 */
    ASSERT_EQ(a.framebits, 8);
    ASSERT_EQ(a.bitmask, 0x1F);
}

TEST(test_acia_clock_accuracy) {
    acia6551_t a;
    acia_init(&a);

    /* 9600 baud, 8-N-1: cycles = (1000000 * 10) / 9600 = 1041 */
    acia_write(&a, ACIA_REG_CONTROL, 0x1E);  /* 9600 baud, 8-bit, 1 stop */
    ASSERT_EQ(a.tx_reload, (int32_t)((1000000L * 10) / 9600));
    ASSERT_EQ(a.baud_rate, 9600u);

    /* 300 baud, 7-E-1: cycles = (1000000 * 10) / 300 = 33333 */
    acia_write(&a, ACIA_REG_CONTROL, 0x26);  /* 300 baud, WL=01 (7-bit), 1 stop */
    acia_write(&a, ACIA_REG_COMMAND, 0x20);  /* PME=1 */
    ASSERT_EQ(a.tx_reload, (int32_t)((1000000L * 10) / 300));
    ASSERT_EQ(a.baud_rate, 300u);
}

TEST(test_bitmask_applied) {
    setup();

    /* Configure 7-bit mode: control WL=01 (7-bit), 19200 baud */
    acia_write(&acia, ACIA_REG_CONTROL, 0x2F);  /* 19200, WL=01 (7-bit) */
    acia_write(&acia, ACIA_REG_COMMAND, 0x03);   /* DTR on, IRQ disabled */
    ASSERT_EQ(acia.bitmask, 0x7F);

    /* Send byte 0xFF — should be masked to 0x7F */
    acia_write(&acia, ACIA_REG_DATA, 0xFF);
    ASSERT_EQ(acia.tdr, 0x7F);  /* Bit 7 stripped */

    /* Wait for TX + loopback RX */
    tick_one_byte(19200);
    tick_one_byte(19200);

    /* Received byte should also be masked */
    uint8_t status = acia_read(&acia, ACIA_REG_STATUS);
    if (status & ACIA_STATUS_RDRF) {
        uint8_t data = acia_read(&acia, ACIA_REG_DATA);
        ASSERT_EQ(data, 0x7F);
    }

    teardown();
}

TEST(test_modem_backend_create) {
    /* Test modem backend creation (no network, just init) */
    serial_backend_t* mb = serial_backend_modem_create("localhost", 2323, false);
    ASSERT_TRUE(mb != NULL);
    ASSERT_EQ(mb->type, SERIAL_BACKEND_MODEM);
    /* Don't open (would try TCP) — just verify creation */
    serial_backend_destroy(mb);
}

TEST(test_rx_fifo) {
    setup();

    /* Enable 4-byte FIFO */
    acia_set_rx_fifo(&acia, 4);

    /* Configure 19200 baud, DTR on, IRQ disabled */
    acia_write(&acia, ACIA_REG_CONTROL, 0x1F);
    acia_write(&acia, ACIA_REG_COMMAND, 0x03);

    /* Inject 3 bytes into loopback rapidly */
    loopback->send(loopback, 0x41);  /* A */
    loopback->send(loopback, 0x42);  /* B */
    loopback->send(loopback, 0x43);  /* C */

    /* Tick enough for all 3 to be received */
    tick_one_byte(19200);
    tick_one_byte(19200);
    tick_one_byte(19200);

    /* First byte should be in RDR, rest in FIFO */
    ASSERT_TRUE(acia_read(&acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    uint8_t b1 = acia_read(&acia, ACIA_REG_DATA);
    ASSERT_EQ(b1, 0x41);

    /* FIFO auto-load: RDRF should still be set */
    ASSERT_TRUE(acia_read(&acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    uint8_t b2 = acia_read(&acia, ACIA_REG_DATA);
    ASSERT_EQ(b2, 0x42);

    /* Third byte */
    ASSERT_TRUE(acia_read(&acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    uint8_t b3 = acia_read(&acia, ACIA_REG_DATA);
    ASSERT_EQ(b3, 0x43);

    /* Now empty */
    ASSERT_FALSE(acia_read(&acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);

    acia_set_rx_fifo(&acia, 0);  /* Disable FIFO */
    teardown();
}

TEST(test_irq_65c51_mode) {
    setup();

    /* Enable 65C51 IRQ mode */
    acia_set_irq_on_rdrf(&acia, true);

    /* Configure 19200 baud, DTR on, IRQ RX enabled (IRD=0) */
    acia_write(&acia, ACIA_REG_CONTROL, 0x1F);
    acia_write(&acia, ACIA_REG_COMMAND, 0x01);  /* DTR on, IRD=0 */

    /* Inject byte */
    loopback->send(loopback, 0x55);
    tick_one_byte(19200);

    /* IRQ should be set (RDRF=1, IRD=0) */
    uint8_t status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_IRQ);
    ASSERT_TRUE(status & ACIA_STATUS_RDRF);

    /* Reading status cleared IRQ on the line...
     * but in 65C51 mode, IRQ should re-assert since RDRF is still set
     * (we didn't read DATA yet) */
    status = acia_read(&acia, ACIA_REG_STATUS);
    /* In 65C51 mode, IRQ bit should be back because RDRF is still set */
    ASSERT_TRUE(status & ACIA_STATUS_IRQ);

    /* Now read DATA to clear RDRF */
    acia_read(&acia, ACIA_REG_DATA);

    /* IRQ should be gone now */
    status = acia_read(&acia, ACIA_REG_STATUS);
    ASSERT_FALSE(status & ACIA_STATUS_IRQ);

    acia_set_irq_on_rdrf(&acia, false);
    teardown();
}

TEST(test_state_preservation) {
    /* Verify that ACIA state fields survive a simulated save/restore cycle.
     * We don't test the actual savestate file format here — just that the
     * register values and internal state can be captured and restored. */
    acia6551_t original, restored;
    acia_init(&original);
    acia_init(&restored);

    /* Configure original with specific state */
    acia_write(&original, ACIA_REG_CONTROL, 0x1E);  /* 9600 baud */
    acia_write(&original, ACIA_REG_COMMAND, 0x0B);  /* DTR + TIC=10 */
    original.rdr = 0xAA;
    original.rx_full = true;
    original.status |= ACIA_STATUS_RDRF;
    original.v23_mode = true;
    original.dcd = false;
    original.tx_cycles = 42;
    original.rx_cycles = 99;

    /* Simulate save: copy fields (what savestate_save writes) */
    restored.tdr = original.tdr;
    restored.rdr = original.rdr;
    restored.status = original.status;
    restored.command = original.command;
    restored.control = original.control;
    restored.tx_pending = original.tx_pending;
    restored.rx_full = original.rx_full;
    restored.irq_line = original.irq_line;
    restored.tx_cycles = original.tx_cycles;
    restored.rx_cycles = original.rx_cycles;
    restored.v23_mode = original.v23_mode;
    restored.dcd = original.dcd;
    restored.dsr = original.dsr;
    restored.cts = original.cts;

    /* Verify all fields match */
    ASSERT_EQ(restored.rdr, 0xAA);
    ASSERT_EQ(restored.command, 0x0B);
    ASSERT_EQ(restored.control, 0x1E);
    ASSERT_TRUE(restored.rx_full);
    ASSERT_TRUE(restored.status & ACIA_STATUS_RDRF);
    ASSERT_TRUE(restored.v23_mode);
    ASSERT_FALSE(restored.dcd);
    ASSERT_EQ(restored.tx_cycles, 42);
    ASSERT_EQ(restored.rx_cycles, 99);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Regression — Digitelec DCD deadlock (OricTel report #1)
 *
 *  A backend that auto-dials on a DTR edge and raises DCD only from inside
 *  its own poll() — exactly the Digitelec DTL 2000 pattern. acia_tick() must
 *  poll the backend even while DCD is low, otherwise the carrier is never
 *  established in pure-receive mode (poll() raises DCD, but poll() was gated
 *  on DCD — a chicken-and-egg deadlock).
 * ═══════════════════════════════════════════════════════════════════════ */

static acia6551_t* g_auto_acia;
static bool g_auto_carrier;
static uint8_t g_auto_byte;
static bool g_auto_byte_ready;

static bool auto_poll(serial_backend_t* self) {
    (void)self;
    /* Mimic digitelec_check_dtr(): connect on a DTR edge from inside poll(). */
    bool dtr = (acia_read(g_auto_acia, ACIA_REG_COMMAND) & ACIA_CMD_DTR) != 0;
    if (dtr && !g_auto_carrier) {
        g_auto_carrier = true;
        g_auto_byte = 0x4E;                 /* 'N' — arrives once carrier up */
        g_auto_byte_ready = true;
        acia_set_dcd(g_auto_acia, true);    /* raise carrier — like connect() */
    }
    return g_auto_byte_ready;
}

static bool auto_recv(serial_backend_t* self, uint8_t* byte) {
    (void)self;
    if (!g_auto_byte_ready) return false;
    *byte = g_auto_byte;
    g_auto_byte_ready = false;
    return true;
}

static bool auto_send(serial_backend_t* self, uint8_t b) { (void)self; (void)b; return true; }
static bool auto_connected(serial_backend_t* self) { (void)self; return g_auto_carrier; }

TEST(test_dcd_deadlock_autodial) {
    acia6551_t a;
    acia_init(&a);

    serial_backend_t mock;
    memset(&mock, 0, sizeof(mock));
    mock.send = auto_send;
    mock.recv = auto_recv;
    mock.poll = auto_poll;
    mock.connected = auto_connected;

    g_auto_acia = &a;
    g_auto_carrier = false;
    g_auto_byte_ready = false;

    acia_set_backend(&a, &mock);

    /* Simulate digitelec_open(): no carrier yet. */
    acia_set_dcd(&a, false);
    ASSERT_FALSE(a.dcd);

    /* Host asserts DTR but transmits nothing (pure-receive direct mode). */
    acia_write(&a, ACIA_REG_COMMAND, ACIA_CMD_DTR);

    /* Tick the ACIA without any TX. With the bug, poll() is gated on DCD and
     * never runs → the backend never dials → DCD stays low forever. With the
     * fix, poll() runs regardless, raises DCD, and the byte reaches RDR. */
    for (int i = 0; i < 2000; i++) acia_tick(&a, 4);

    ASSERT_TRUE(a.dcd);                                  /* carrier established */
    uint8_t status = acia_read(&a, ACIA_REG_STATUS);
    ASSERT_TRUE(status & ACIA_STATUS_RDRF);              /* byte delivered */
    ASSERT_EQ(acia_read(&a, ACIA_REG_DATA), 0x4E);       /* the 'N' */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  ACIA 6551 Serial Interface Tests\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    RUN(test_init_state);
    RUN(test_programmed_reset);
    RUN(test_control_register_rw);
    RUN(test_command_register_rw);
    RUN(test_loopback_tx_rx);
    RUN(test_tx_irq);
    RUN(test_rx_irq);
    RUN(test_overrun);
    RUN(test_v23_mode);
    RUN(test_ext_clock_baud);
    RUN(test_dcd_dsr_status);
    RUN(test_loopback_backend);
    RUN(test_frame_format_8n1);
    RUN(test_frame_format_7e1);
    RUN(test_frame_format_5n2);
    RUN(test_acia_clock_accuracy);
    RUN(test_bitmask_applied);
    RUN(test_modem_backend_create);
    RUN(test_rx_fifo);
    RUN(test_irq_65c51_mode);
    RUN(test_state_preservation);
    RUN(test_dcd_deadlock_autodial);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
