/**
 * @file test_mea8000.c
 * @brief Philips MEA 8000 (TMPI "Synthétiseur Vocal") unit tests
 * @author bmarty <bmarty@mailo.com>
 *
 * Deterministic: the formant synth has no ROM and its noise table is seeded from
 * a fixed PRNG, so every run is reproducible. Voiced frames use the sawtooth
 * source; the tests feed hand-built frames and check the state machine, the
 * ready/STATUS handshake, and that synthesis produces (then stops) audio.
 */

#include <stdio.h>
#include <string.h>
#include "io/mea8000.h"

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
        printf("FAIL\n    %s:%d: expected 0x%llX, got 0x%llX\n", __FILE__, __LINE__, \
               (unsigned long long)(b), (unsigned long long)(a)); \
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
    if (x) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* A plausible voiced frame: mixed bandwidths, fm1~415 / fm2~784 / fm3~2047,
 * amplitude ~500, 64 ms, small positive pitch increment. */
static const uint8_t VOICED_FRAME[4] = { 0x1B, 0x8A, 0x7E, 0xE4 };

static void feed_frame(mea8000_t* m, uint8_t pitch, const uint8_t f[4]) {
    mea8000_write(m, 0x03F0, pitch);            /* initial pitch byte */
    for (int i = 0; i < 4; i++)
        mea8000_write(m, 0x03F0, f[i]);         /* 4 frame bytes */
}

/* ── init / reset ──────────────────────────────────────────────────────── */
TEST(test_init_defaults) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    ASSERT_EQ(m.base_addr, 0x03F0);
    ASSERT_EQ(m.state, MEA8000_STOPPED);
    ASSERT_TRUE(m.tables_ready);
    ASSERT_FALSE(mea8000_speaking(&m));
}

TEST(test_init_default_addr_when_zero) {
    mea8000_t m;
    mea8000_init(&m, 0);
    ASSERT_EQ(m.base_addr, MEA8000_BASE_DEFAULT);
}

/* ── status register ───────────────────────────────────────────────────── */
TEST(test_status_idle_ready) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    ASSERT_EQ(mea8000_read(&m, 0x03F0), 0x80);   /* D7=1 ready (data addr)    */
    ASSERT_EQ(mea8000_read(&m, 0x03F1), 0x80);   /* D7=1 ready (command addr) */
}

TEST(test_read_wrong_addr) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    ASSERT_EQ(mea8000_read(&m, 0x0300), 0xFF);
}

/* ── frame loading state machine ───────────────────────────────────────── */
TEST(test_pitch_moves_to_wait_first) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    mea8000_write(&m, 0x03F0, 60);
    ASSERT_EQ(m.state, MEA8000_WAIT_FIRST);
    ASSERT_EQ(m.pitch, 120);                     /* 2 × 60 */
}

TEST(test_full_frame_starts) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    feed_frame(&m, 60, VOICED_FRAME);
    ASSERT_EQ(m.state, MEA8000_STARTED);
    ASSERT_TRUE(mea8000_speaking(&m));
    ASSERT_EQ(m.bufpos, 0);                       /* frame consumed → ready   */
}

TEST(test_ready_toggles_with_buffer) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    feed_frame(&m, 60, VOICED_FRAME);
    ASSERT_EQ(mea8000_read(&m, 0x03F0), 0x80);    /* STARTED, bufpos<4 → ready */
    /* queue a full successor frame → buffer full → not ready */
    for (int i = 0; i < 4; i++) mea8000_write(&m, 0x03F0, VOICED_FRAME[i]);
    ASSERT_EQ(m.bufpos, 4);
    ASSERT_EQ(mea8000_read(&m, 0x03F0), 0x00);    /* D7=0 busy */
}

TEST(test_command_stop) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    feed_frame(&m, 60, VOICED_FRAME);
    ASSERT_TRUE(mea8000_speaking(&m));
    mea8000_write(&m, 0x03F1, 0x10);              /* command: STOP (bit4) */
    ASSERT_EQ(m.state, MEA8000_STOPPED);
    ASSERT_FALSE(mea8000_speaking(&m));
}

TEST(test_buffer_overflow_safe) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    feed_frame(&m, 60, VOICED_FRAME);
    for (int i = 0; i < 4; i++) mea8000_write(&m, 0x03F0, 0x11);  /* fill buffer */
    mea8000_write(&m, 0x03F0, 0x22);              /* 5th byte: must be dropped, no crash */
    ASSERT_EQ(m.bufpos, 4);
}

/* ── synthesis ─────────────────────────────────────────────────────────── */
TEST(test_generate_silence_when_idle) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    int16_t out[882];
    mea8000_tick(&m, 19968);
    mea8000_generate(&m, out, 882);
    int nz = 0;
    for (int i = 0; i < 882; i++) if (out[i]) nz++;
    ASSERT_EQ(nz, 0);
}

TEST(test_voiced_frame_produces_audio_then_idle) {
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    feed_frame(&m, 60, VOICED_FRAME);

    int16_t out[882];
    int max_abs = 0, idle_frame = -1;
    for (int f = 0; f < 20; f++) {
        mea8000_tick(&m, 19968);
        mea8000_generate(&m, out, 882);
        for (int i = 0; i < 882; i++) {
            int a = out[i] < 0 ? -out[i] : out[i];
            if (a > max_abs) max_abs = a;
        }
        if (!mea8000_speaking(&m) && f > 0) { idle_frame = f; break; }
    }
    ASSERT_TRUE(max_abs > 500);          /* audible formant output */
    ASSERT_TRUE(idle_frame > 0);         /* slow-stop → returns to idle */
    printf("(peak=%d, idle@%d)  ", max_abs, idle_frame);
}

TEST(test_bonjour_sequence) {
    /* The MAGECO board self-test's "BONJOUR" byte list (pitch + frame data).
     * We only assert it drives the chip to produce audio then settle — the exact
     * intelligibility depends on the driver's framing. */
    mea8000_t m;
    mea8000_init(&m, 0x03F0);
    const uint8_t bonjour[] = { 0, 63, 23, 38, 31, 39, 4, 4, 4, 4 };
    for (size_t i = 0; i < sizeof(bonjour); i++)
        mea8000_write(&m, 0x03F0, bonjour[i]);

    int16_t out[882];
    int max_abs = 0;
    for (int f = 0; f < 30; f++) {
        mea8000_tick(&m, 19968);
        mea8000_generate(&m, out, 882);
        for (int i = 0; i < 882; i++) {
            int a = out[i] < 0 ? -out[i] : out[i];
            if (a > max_abs) max_abs = a;
        }
        if (!mea8000_speaking(&m) && f > 2) break;
    }
    ASSERT_TRUE(max_abs > 200);          /* produced some speech output */
    ASSERT_FALSE(mea8000_speaking(&m));  /* eventually idle */
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  MEA 8000 (TMPI speech synth) tests\n");
    printf("═══════════════════════════════════════════════════════\n");

    RUN(test_init_defaults);
    RUN(test_init_default_addr_when_zero);
    RUN(test_status_idle_ready);
    RUN(test_read_wrong_addr);
    RUN(test_pitch_moves_to_wait_first);
    RUN(test_full_frame_starts);
    RUN(test_ready_toggles_with_buffer);
    RUN(test_command_stop);
    RUN(test_buffer_overflow_safe);
    RUN(test_generate_silence_when_idle);
    RUN(test_voiced_frame_produces_audio_then_idle);
    RUN(test_bonjour_sequence);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
