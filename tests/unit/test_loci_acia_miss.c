/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_loci_acia_miss.c
 * @brief Faithful reproduction of the LOCI PHI2 race on the ACIA at $0380 (picowifi).
 * @author bmarty <bmarty@mailo.com>
 *
 * Checks the "lost serve race" model of io_bus.c (acia_dev_read/write/peek):
 *
 *  - VIA inhibited symmetrically → a miss returns OPEN-BUS (last byte on the data
 *    bus), not the VIA nor 0xFF (see extensions/analyse/read-serve-et-inhibition-via.md).
 *  - Missed DATA read = DESTRUCTIVE on the LOCI side: the RX byte is consumed "blindly"
 *    and lost → "modem unreachable" (the core of the bug).
 *  - Missed STAT/CMD/CTRL read = IDEMPOTENT: open-bus this time round, but the register
 *    can be re-read on the next one (miss forgiven, like disk/MIA polling).
 *  - WRITE always reliable: reaches the ACIA even when the race is lost.
 *  - peek() (observer): open-bus WITHOUT consuming.
 *
 * All of it is DETERMINISTIC: the reliable `tior` window drives the miss (no randomness).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emulator.h"
#include "io/io_bus.h"
#include "io/acia6551.h"
#include "io/serial_backend.h"
#include "io/loci.h"
#include "io/bus_timing.h"
#include "memory/memory.h"

/* ── Micro-framework (identical to the other suites) ───────────────────────── */
static int tests_passed = 0;
static int tests_failed = 0;
#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-45s", #name); name(); \
    printf("\n"); } while (0)
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { printf("[FAIL] %s:%d %s", __FILE__, __LINE__, #cond); \
        tests_failed++; return; } } while (0)
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) do { \
    long _va = (long)(a), _vb = (long)(b); \
    if (_va != _vb) { printf("[FAIL] %s:%d %s==%s (%ld != %ld)", \
        __FILE__, __LINE__, #a, #b, _va, _vb); tests_failed++; return; } } while (0)
#define PASS() do { tests_passed++; printf("[OK]"); } while (0)

/* ── Harness: minimal LOCI emulator + ACIA at $0380 ────────────────────────── */
static emulator_t*      g_emu = NULL;
static serial_backend_t* g_loop = NULL;

static void setup(void) {
    g_emu = (emulator_t*)calloc(1, sizeof(emulator_t));
    memory_init(&g_emu->memory);
    acia_init(&g_emu->acia);
    g_loop = serial_backend_loopback_create();
    g_loop->open(g_loop);
    acia_set_backend(&g_emu->acia, g_loop);
    /* 19200 8-N-1, DTR on: enables loopback reception. */
    acia_write(&g_emu->acia, ACIA_REG_CONTROL, 0x1F);
    acia_write(&g_emu->acia, ACIA_REG_COMMAND, 0x01);

    g_emu->card_on[CARD_IDX_acia] = true;
    g_emu->acia_base_addr = 0x0380;
    g_emu->has_loci = true;
    /* Reliable window [5,10]: tior=0 (default) → OUTSIDE the window → race lost. */
    loci_set_mia_window(&g_emu->loci, 5, 10);
    g_emu->loci.mia_tior = 0;
}

static void teardown(void) {
    if (g_loop) { serial_backend_destroy(g_loop); g_loop = NULL; }
    if (g_emu) { memory_cleanup(&g_emu->memory); free(g_emu); g_emu = NULL; }
}

static void set_reliable(bool reliable) {
    g_emu->loci.mia_tior = reliable ? 7 : 0;   /* 7 ∈ [5,10], 0 ∉ */
}

/* Injects a byte into the RX stream (via the loopback backend) and brings it up
 * into the ACIA (RDRF set). */
static void inject_rx(uint8_t byte) {
    g_loop->send(g_loop, byte);
    for (int i = 0; i < 800; i++) acia_tick(&g_emu->acia, 4);
}

/* Read/write via the real I/O bus (io_bus dispatch), like the 6502. */
static uint8_t bus_read(uint16_t addr) {
    const io_device_t* d = io_bus_find(g_emu, addr);
    return (d && d->read) ? d->read(g_emu, addr) : 0;
}
static uint8_t bus_peek(uint16_t addr) {
    const io_device_t* d = io_bus_find(g_emu, addr);
    return (d && d->peek) ? d->peek(g_emu, addr) : (d ? d->read(g_emu, addr) : 0);
}
static bool bus_write(uint16_t addr, uint8_t v) {
    const io_device_t* d = io_bus_find_write(g_emu, addr);
    return d && d->write ? d->write(g_emu, addr, v) : false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

/* $0380 is indeed claimed by the ACIA (not the LOCI: outside the MIA/TAP/DSK window). */
TEST(test_acia_claims_0380) {
    setup();
    const io_device_t* d = io_bus_find(g_emu, 0x0380);
    ASSERT_TRUE(d != NULL);
    ASSERT_TRUE(strcmp(d->name, "acia") == 0);
    teardown();
    PASS();
}

/* Race lost: DATA read returns open-bus (last byte on the bus), NOT the
 * VIA nor 0xFF, and the RX byte is CONSUMED (lost) → RDRF drops. */
TEST(test_data_miss_is_open_bus_and_destructive) {
    setup();
    set_reliable(false);
    inject_rx(0xAA);
    ASSERT_TRUE(acia_peek(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);

    g_emu->memory.last_bus_value = 0x3C;         /* bus residue (open-bus expected) */
    uint8_t got = bus_read(0x0380);
    ASSERT_EQ(got, 0x3C);                          /* open-bus, neither 0xAA nor 0xFF */

    /* The byte was consumed blindly → lost. Once reliable again, no more RDRF. */
    set_reliable(true);
    ASSERT_FALSE(acia_read(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    teardown();
    PASS();
}

/* Race lost: STATUS read returns open-bus but does NOT consume (idempotent) →
 * the RX byte survives and remains readable once reliability is restored. */
TEST(test_status_miss_is_idempotent) {
    setup();
    set_reliable(false);
    inject_rx(0xBB);

    g_emu->memory.last_bus_value = 0x11;
    uint8_t st = bus_read(0x0381);                 /* STATUS missed */
    ASSERT_EQ(st, 0x11);                            /* open-bus */

    /* RX not stolen: reliable again, RDRF still there, DATA reads back 0xBB. */
    set_reliable(true);
    ASSERT_TRUE(acia_read(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    ASSERT_EQ(acia_read(&g_emu->acia, ACIA_REG_DATA), 0xBB);
    teardown();
    PASS();
}

/* The write is always reliable: it reaches the ACIA even when the race is lost. */
TEST(test_write_always_reaches_acia) {
    setup();
    set_reliable(false);
    ASSERT_TRUE(bus_write(0x0382, 0x0B));          /* COMMAND = DTR|TIC (race lost) */

    set_reliable(true);
    ASSERT_EQ(acia_read(&g_emu->acia, ACIA_REG_COMMAND), 0x0B);
    teardown();
    PASS();
}

/* peek() (observer: debugger/monitor) returns open-bus WITHOUT consuming the RX. */
TEST(test_peek_miss_open_bus_non_destructive) {
    setup();
    set_reliable(false);
    inject_rx(0xCC);

    g_emu->memory.last_bus_value = 0x22;
    ASSERT_EQ(bus_peek(0x0380), 0x22);             /* open-bus, non-destructive */

    set_reliable(true);
    ASSERT_TRUE(acia_read(&g_emu->acia, ACIA_REG_STATUS) & ACIA_STATUS_RDRF);
    ASSERT_EQ(acia_read(&g_emu->acia, ACIA_REG_DATA), 0xCC);  /* byte preserved */
    teardown();
    PASS();
}

/* Reliable window: 6551 behaviour strictly unchanged (no regression). */
TEST(test_reliable_read_is_pristine) {
    setup();
    set_reliable(true);
    inject_rx(0xDD);
    g_emu->memory.last_bus_value = 0x99;           /* must NOT leak when reliable */
    ASSERT_TRUE(bus_read(0x0381) & ACIA_STATUS_RDRF);
    ASSERT_EQ(bus_read(0x0380), 0xDD);             /* real data, not open-bus */
    teardown();
    PASS();
}

/* Without LOCI (standalone ACIA), no race: read always clean even with tior=0. */
TEST(test_no_loci_no_race) {
    setup();
    g_emu->has_loci = false;                        /* no MIA → no fragile serve */
    g_emu->loci.mia_tior = 0;
    inject_rx(0xEE);
    g_emu->memory.last_bus_value = 0x55;
    ASSERT_EQ(bus_read(0x0380), 0xEE);             /* real data, open-bus ignored */
    teardown();
    PASS();
}

/* ── Epic B / Phase 1: sub-cycle PHI2 race model (physically grounded) ── */

/* The serve arrives at subtick (tior + serve_subticks); clean iff ≤ latch. */
TEST(test_phase_model_serve_race) {
    setup();
    loci_set_serve_timing(&g_emu->loci, 20, 27);   /* serve=20, latch=27 subticks */

    g_emu->loci.mia_tior = 0;                        /* valid=20 ≤ 27 → clean */
    ASSERT_TRUE(loci_mia_io_reliable(&g_emu->loci));
    inject_rx(0x7E);
    ASSERT_EQ(bus_read(0x0380), 0x7E);              /* real data */

    g_emu->loci.mia_tior = 8;                        /* valid=28 > 27 → race lost */
    ASSERT_FALSE(loci_mia_io_reliable(&g_emu->loci));
    inject_rx(0x99);
    g_emu->memory.last_bus_value = 0x44;
    ASSERT_EQ(bus_read(0x0380), 0x44);              /* open-bus, byte lost */
    teardown();
    PASS();
}

/* Reproduces the bug report: same board, `-Os` build (short serve) works,
 * `-O2` build (long serve) misses — independently of any tior setting. */
TEST(test_phase_reproduces_build_os_vs_o2) {
    setup();
    g_emu->loci.mia_tior = 0;

    loci_set_serve_timing(&g_emu->loci, 26, 27);   /* -Os: serve 26 cyc ≤ latch → OK */
    ASSERT_TRUE(loci_mia_io_reliable(&g_emu->loci));

    loci_set_serve_timing(&g_emu->loci, 36, 27);   /* -O2: serve 36 cyc > latch → KO */
    ASSERT_FALSE(loci_mia_io_reliable(&g_emu->loci));
    teardown();
    PASS();
}

/* The raw race predicate (bus_timing.h): valid ≤ latch wins. */
TEST(test_bus_serve_wins_race_predicate) {
    ASSERT_TRUE(bus_serve_wins_race(0, 27));         /* on-board: always */
    ASSERT_TRUE(bus_serve_wins_race(27, 27));        /* exactly at the latch */
    ASSERT_FALSE(bus_serve_wins_race(28, 27));       /* misses by one subtick */
    PASS();
}

/* ── Phase 2: seeded jitter — occasional, deterministic misses ── */

/* Counts the misses over N accesses (via the CPU path, which samples the jitter). */
static int count_losses(int n) {
    int lost = 0;
    for (int i = 0; i < n; i++)
        if (loci_mia_serve_lost_sampled(&g_emu->loci)) lost++;
    return lost;
}

/* Right on the boundary (tior+serve == latch): without jitter everything passes; with
 * symmetric jitter, a SHARE of accesses miss (occasional, not all-or-nothing). */
TEST(test_jitter_makes_losses_occasional) {
    setup();
    loci_set_serve_timing(&g_emu->loci, 27, 27);     /* nominal exactly at the latch → clean */
    g_emu->loci.mia_tior = 0;
    ASSERT_EQ(count_losses(200), 0);                  /* without jitter: never missed */

    loci_set_serve_jitter(&g_emu->loci, 3, 12345);    /* ±3 subticks */
    int lost = count_losses(200);
    ASSERT_TRUE(lost > 0 && lost < 200);              /* mix of clean/missed */
    teardown();
    PASS();
}

/* Reproducibility: same seed → same exact sequence of misses. */
TEST(test_jitter_is_deterministic_per_seed) {
    setup();
    loci_set_serve_timing(&g_emu->loci, 27, 27);
    loci_set_serve_jitter(&g_emu->loci, 3, 999);
    int a = count_losses(100);
    loci_set_serve_jitter(&g_emu->loci, 3, 999);      /* identical re-seed */
    int b = count_losses(100);
    ASSERT_EQ(a, b);                                   /* same seed → identical sequence */
    teardown();
    PASS();
}

/* The jitter only affects the CPU path: peek (observer) stays on the const
 * nominal, without advancing the PRNG or stealing a byte. */
TEST(test_jitter_peek_uses_nominal) {
    setup();
    loci_set_serve_timing(&g_emu->loci, 20, 27);      /* nominal comfortably clean */
    loci_set_serve_jitter(&g_emu->loci, 3, 7);
    inject_rx(0xC3);
    ASSERT_EQ(bus_peek(0x0380), 0xC3);                /* nominal peek: real data */
    teardown();
    PASS();
}

int main(void) {
    printf("\n=== LOCI ACIA $0380 — course PHI2 (picowifi) ===\n");
    RUN(test_acia_claims_0380);
    RUN(test_data_miss_is_open_bus_and_destructive);
    RUN(test_status_miss_is_idempotent);
    RUN(test_write_always_reaches_acia);
    RUN(test_peek_miss_open_bus_non_destructive);
    RUN(test_reliable_read_is_pristine);
    RUN(test_no_loci_no_race);
    RUN(test_phase_model_serve_race);
    RUN(test_phase_reproduces_build_os_vs_o2);
    RUN(test_bus_serve_wins_race_predicate);
    RUN(test_jitter_makes_losses_occasional);
    RUN(test_jitter_is_deterministic_per_seed);
    RUN(test_jitter_peek_uses_nominal);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
