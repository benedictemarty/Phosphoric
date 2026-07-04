/**
 * @file test_cassette.c
 * @brief Signal-level cassette generator unit tests (Sprint 90)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-07-04
 * @version 1.0.0-alpha
 *
 * Validates the tape waveform generator that drives VIA CB1: frame encoding,
 * motor gating, rewind, and a full round-trip — generate the waveform, sample
 * the CB1 rising edges through a real via6522_t, classify each pulse by width
 * exactly like the ROM ($E67D: interval < ~512 cycles => short/'1'), and check
 * the bytes decode back identically. This is the same signal the real ORIC ROM
 * CLOAD reads in --tape-signal mode.
 */

#include <stdio.h>
#include <string.h>
#include "io/cassette.h"
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

/* ── Frame encoding ─────────────────────────────────────────────────── */

TEST(test_encode_frame_start_and_stops) {
    /* Every frame: start bit (0) low, four stop bits (1) high. */
    uint16_t f = cassette_encode_frame(0x00);
    ASSERT_EQ(f & 0x0001u, 0u);         /* start bit = 0 */
    ASSERT_EQ((f >> 10) & 0x0Fu, 0x0Fu); /* four stop bits = 1 */
}

TEST(test_encode_frame_data_lsb_first) {
    /* Data bits occupy positions 1..8, LSB first. 0x16 = 0b00010110. */
    uint16_t f = cassette_encode_frame(0x16);
    uint8_t data = (uint8_t)((f >> 1) & 0xFFu);
    ASSERT_EQ(data, 0x16u);
}

TEST(test_encode_frame_odd_parity) {
    /* Parity bit (position 9) = (number of data ones) & 1. */
    ASSERT_EQ((cassette_encode_frame(0x16) >> 9) & 1u, 1u); /* 0x16 has 3 ones -> 1 */
    ASSERT_EQ((cassette_encode_frame(0x00) >> 9) & 1u, 0u); /* 0 ones -> 0 */
    ASSERT_EQ((cassette_encode_frame(0xFF) >> 9) & 1u, 0u); /* 8 ones -> 0 */
    ASSERT_EQ((cassette_encode_frame(0x01) >> 9) & 1u, 1u); /* 1 one  -> 1 */
}

/* ── Init / motor / rewind ──────────────────────────────────────────── */

TEST(test_init_idle) {
    cassette_t c;
    cassette_init(&c);
    ASSERT_FALSE(c.signal_mode);
    ASSERT_FALSE(c.motor_on);
    ASSERT_FALSE(c.finished);
    ASSERT_FALSE(c.started);
}

TEST(test_motor_gate_no_edges_when_off) {
    /* With the motor off, ticking must not toggle CB1. */
    static const uint8_t buf[] = { 0x16, 0x24, 0x41 };
    via6522_t via; via_init(&via); via_reset(&via);
    via_write(&via, VIA_PCR, 0x10);
    cassette_t c; cassette_init(&c);
    cassette_signal_begin(&c, buf, (int)sizeof(buf));
    cassette_set_motor(&c, false);
    via.ifr = 0;
    for (int i = 0; i < 10000; i++) cassette_tick(&c, &via, 4);
    ASSERT_EQ(via.ifr & VIA_INT_CB1, 0u);   /* no CB1 activity */
}

/* Round-trip helper: play the waveform, classify pulses like the ROM, and
 * frame-decode. Returns the number of decoded bytes (leader syncs included). */
static int roundtrip(const uint8_t* buf, int len, uint8_t* out, int outmax) {
    via6522_t via; via_init(&via); via_reset(&via);
    via_write(&via, VIA_PCR, 0x10);         /* CB1 interrupt on rising edge */

    cassette_t c; cassette_init(&c);
    cassette_signal_begin(&c, buf, len);
    cassette_set_motor(&c, true);

    static int bits[8192];
    int nbits = 0;
    unsigned long long cyc = 0, last = 0;
    int have_last = 0, safety = 0;

    while (!c.finished && nbits < (int)(sizeof(bits)/sizeof(bits[0])) &&
           safety++ < 5000000) {
        cassette_tick(&c, &via, 2);
        cyc += 2;
        if (via.ifr & VIA_INT_CB1) {
            via.ifr &= (uint8_t)~VIA_INT_CB1;   /* ROM clears by reading ORB */
            if (have_last) {
                unsigned long long iv = cyc - last;
                bits[nbits++] = (iv < 512) ? 1 : 0;  /* ROM $E67D threshold */
            }
            last = cyc;
            have_last = 1;
        }
    }

    /* Frame-aligned decode: bit 0 is the first frame's start bit; every 14 bits
     * is one frame [start, 8 data LSB, parity, 4 stops]. */
    int nb = 0;
    for (int f = 0; f * CAS_FRAME_BITS + 9 <= nbits && nb < outmax; f++) {
        int base = f * CAS_FRAME_BITS;
        uint8_t byte = 0;
        for (int i = 0; i < 8; i++) byte |= (uint8_t)(bits[base + 1 + i] << i);
        out[nb++] = byte;
    }
    return nb;
}

TEST(test_roundtrip_leader_is_sync_bytes) {
    static const uint8_t buf[] = { 0x16, 0x16, 0x16, 0x24, 0x00 };
    static uint8_t out[256];
    int n = roundtrip(buf, (int)sizeof(buf), out, 256);
    ASSERT_TRUE(n > CAS_LEADER_SYNCS);
    /* The pilot leader is CAS_LEADER_SYNCS frames of 0x16. */
    for (int i = 0; i < CAS_LEADER_SYNCS; i++)
        ASSERT_EQ(out[i], 0x16u);
}

TEST(test_roundtrip_payload_matches) {
    /* A representative byte spread: sync, marker, extremes, alternating bits. */
    static const uint8_t buf[] = {
        0x16, 0x24, 0x00, 0xFF, 0x55, 0xAA, 0x01, 0x80, 0x7F, 0x42, 0x3C
    };
    static uint8_t out[256];
    int n = roundtrip(buf, (int)sizeof(buf), out, 256);
    ASSERT_TRUE(n >= CAS_LEADER_SYNCS + (int)sizeof(buf));
    /* Payload frames follow the leader, byte-identical. */
    for (int i = 0; i < (int)sizeof(buf); i++)
        ASSERT_EQ(out[CAS_LEADER_SYNCS + i], buf[i]);
}

TEST(test_roundtrip_pulse_widths_distinguish_bits) {
    /* All-zero and all-one bytes must decode cleanly (long vs short pulses). */
    static const uint8_t buf[] = { 0x00, 0xFF, 0x00, 0xFF };
    static uint8_t out[256];
    int n = roundtrip(buf, (int)sizeof(buf), out, 256);
    ASSERT_TRUE(n >= CAS_LEADER_SYNCS + 4);
    ASSERT_EQ(out[CAS_LEADER_SYNCS + 0], 0x00u);
    ASSERT_EQ(out[CAS_LEADER_SYNCS + 1], 0xFFu);
    ASSERT_EQ(out[CAS_LEADER_SYNCS + 2], 0x00u);
    ASSERT_EQ(out[CAS_LEADER_SYNCS + 3], 0xFFu);
}

int main(void) {
    printf("\n=== Signal-level Cassette Tests (Sprint 90) ===\n\n");

    RUN(test_encode_frame_start_and_stops);
    RUN(test_encode_frame_data_lsb_first);
    RUN(test_encode_frame_odd_parity);
    RUN(test_init_idle);
    RUN(test_motor_gate_no_edges_when_off);
    RUN(test_roundtrip_leader_is_sync_bytes);
    RUN(test_roundtrip_payload_matches);
    RUN(test_roundtrip_pulse_widths_distinguish_bits);

    printf("\n  Results: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
