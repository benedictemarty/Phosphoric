/**
 * @file test_audio.c
 * @brief AY-3-8910 PSG audio unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.0.0-rc
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "audio/audio.h"

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

/* Helper: write a value to a PSG register */
static void ay_write_reg(ay3891x_t* ay, uint8_t reg, uint8_t val) {
    ay_write_address(ay, reg);
    ay_write_data(ay, val);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 1: INIT STATE                                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_init) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);

    /* All sound registers should be 0 */
    for (int i = 0; i < 14; i++) {
        ASSERT_EQ(ay.registers[i], 0);
    }
    /* Port A and B default to 0xFF (no keys pressed) */
    ASSERT_EQ(ay.registers[14], 0xFF);
    ASSERT_EQ(ay.registers[15], 0xFF);

    /* Noise LFSR seeded to 1 */
    ASSERT_EQ(ay.noise_shift, 1);

    /* Clock rate stored */
    ASSERT_EQ(ay.clock_rate, 1000000);

    /* Envelope state clean */
    ASSERT_EQ(ay.env_step, 0);
    ASSERT_EQ(ay.env_shape, 0);
    ASSERT_FALSE(ay.env_holding);

    /* Tone generators at zero */
    for (int ch = 0; ch < 3; ch++) {
        ASSERT_EQ(ay.tone_period[ch], 0);
        ASSERT_EQ(ay.tone_counter[ch], 0);
        ASSERT_EQ(ay.tone_output[ch], 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 2: WRITE/READ REGISTERS                                      */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_write_read) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);

    /* Write tone period channel A (reg 0 = fine, reg 1 = coarse) */
    ay_write_reg(&ay, 0, 0xAB);
    ay_write_reg(&ay, 1, 0x03);

    /* Read back raw register values */
    ay_write_address(&ay, 0);
    ASSERT_EQ(ay_read_data(&ay), 0xAB);
    ay_write_address(&ay, 1);
    ASSERT_EQ(ay_read_data(&ay), 0x03);

    /* Check computed tone period (12-bit: coarse[3:0] << 8 | fine) */
    ASSERT_EQ(ay.tone_period[0], 0x03AB);

    /* Write noise period (5-bit) */
    ay_write_reg(&ay, 6, 0xFF);  /* Should mask to 5 bits */
    ASSERT_EQ(ay.noise_period, 0x1F);

    /* Write mixer */
    ay_write_reg(&ay, 7, 0x38);  /* Noise off for all, tone on for all */
    ay_write_address(&ay, 7);
    ASSERT_EQ(ay_read_data(&ay), 0x38);

    /* Write envelope period */
    ay_write_reg(&ay, 11, 0x00);
    ay_write_reg(&ay, 12, 0x10);
    ASSERT_EQ(ay.env_period, 0x1000);

    /* Address register wraps at 4 bits */
    ay_write_address(&ay, 0x1F);
    ASSERT_EQ(ay.selected_reg, 0x0F);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 3: ENVELOPE SHAPE 0 (single decay → silence)                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_envelope_shape0) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);

    /* Set envelope period to something small for quick cycle */
    ay_write_reg(&ay, 11, 1);  /* Fine period */
    ay_write_reg(&ay, 12, 0);  /* Coarse period */

    /* Shape 0: single decay (CONTINUE=0, ATTACK=0) */
    ay_write_reg(&ay, 13, 0);

    /* Generate enough samples to complete the envelope cycle */
    int16_t buf[4096];
    ay_generate(&ay, buf, 2048);

    /* After generating enough samples, envelope should be holding */
    ASSERT_TRUE(ay.env_holding);

    /* env_step should be 31 (held at end of single cycle) */
    ASSERT_EQ(ay.env_step, 31);

    /* Envelope volume should be 0 (silent after decay) */
    ASSERT_EQ(ay.env_volume, 0);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 4: ENVELOPE SHAPE 8 (continuous attack cycle)                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_envelope_shape8) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);

    ay_write_reg(&ay, 11, 1);
    ay_write_reg(&ay, 12, 0);

    /* Shape 8: CONTINUE=1, ATTACK=0, ALT=0, HOLD=0 → repeating decay */
    ay_write_reg(&ay, 13, 8);

    /* Generate samples */
    int16_t buf[4096];
    ay_generate(&ay, buf, 2048);

    /* Shape 8 cycles continuously, should NOT be holding */
    ASSERT_FALSE(ay.env_holding);

    /* env_step should wrap (be < 32) */
    ASSERT_TRUE(ay.env_step < 32);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 5: ENVELOPE HOLD SHAPES (9, 11, 13, 15)                      */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_envelope_hold) {
    ay3891x_t ay;
    int16_t buf[4096];

    /* Shape 9: CONTINUE=1, ATTACK=0, ALT=0, HOLD=1 → decay then hold at 0 */
    ay_init(&ay, 1000000);
    ay_write_reg(&ay, 11, 1);
    ay_write_reg(&ay, 12, 0);
    ay_write_reg(&ay, 13, 9);
    ay_generate(&ay, buf, 2048);
    ASSERT_TRUE(ay.env_holding);

    /* Shape 11: CONTINUE=1, ATTACK=0, ALT=1, HOLD=1 → decay, hold at 15 */
    ay_init(&ay, 1000000);
    ay_write_reg(&ay, 11, 1);
    ay_write_reg(&ay, 12, 0);
    ay_write_reg(&ay, 13, 11);
    ay_generate(&ay, buf, 2048);
    ASSERT_TRUE(ay.env_holding);

    /* Shape 13: CONTINUE=1, ATTACK=1, ALT=0, HOLD=1 → attack, hold at 15 */
    ay_init(&ay, 1000000);
    ay_write_reg(&ay, 11, 1);
    ay_write_reg(&ay, 12, 0);
    ay_write_reg(&ay, 13, 13);
    ay_generate(&ay, buf, 2048);
    ASSERT_TRUE(ay.env_holding);

    /* Shape 15: CONTINUE=1, ATTACK=1, ALT=1, HOLD=1 → attack, hold at 0 */
    ay_init(&ay, 1000000);
    ay_write_reg(&ay, 11, 1);
    ay_write_reg(&ay, 12, 0);
    ay_write_reg(&ay, 13, 15);
    ay_generate(&ay, buf, 2048);
    ASSERT_TRUE(ay.env_holding);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 6: GENERATE SILENCE (all volumes 0)                          */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_generate_silence) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);

    /* All volumes at 0, mixer enables tone (bits 0-2 = 0) */
    ay_write_reg(&ay, 7, 0x38);  /* Tone enabled, noise disabled */
    ay_write_reg(&ay, 8, 0);     /* Chan A vol = 0 */
    ay_write_reg(&ay, 9, 0);     /* Chan B vol = 0 */
    ay_write_reg(&ay, 10, 0);    /* Chan C vol = 0 */

    /* Set some tone periods */
    ay_write_reg(&ay, 0, 100);
    ay_write_reg(&ay, 2, 100);
    ay_write_reg(&ay, 4, 100);

    int16_t buf[512];
    memset(buf, 0xAA, sizeof(buf));
    ay_generate(&ay, buf, 256);

    /* All samples should be 0 (silent) */
    int all_silent = 1;
    for (int i = 0; i < 512; i++) {
        if (buf[i] != 0) { all_silent = 0; break; }
    }
    ASSERT_TRUE(all_silent);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 7: GENERATE TONE (volume > 0 → non-zero output)              */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_generate_tone) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);

    /* Enable tone on channel A, disable noise on all */
    ay_write_reg(&ay, 7, 0x3E);  /* Only channel A tone enabled */

    /* Channel A: period=100, volume=15 (max) */
    ay_write_reg(&ay, 0, 100);
    ay_write_reg(&ay, 1, 0);
    ay_write_reg(&ay, 8, 15);

    int16_t buf[2048];
    memset(buf, 0, sizeof(buf));
    ay_generate(&ay, buf, 1024);

    /* Buffer should contain non-zero samples (audible tone) */
    int has_nonzero = 0;
    for (int i = 0; i < 2048; i++) {
        if (buf[i] != 0) { has_nonzero = 1; break; }
    }
    ASSERT_TRUE(has_nonzero);

    /* Verify stereo: left and right should be identical (mono mix) */
    int stereo_match = 1;
    for (int i = 0; i < 1024; i++) {
        if (buf[i * 2] != buf[i * 2 + 1]) { stereo_match = 0; break; }
    }
    ASSERT_TRUE(stereo_match);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 8: MIXER CONTROL                                              */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ay_mixer) {
    ay3891x_t ay;
    int16_t buf[512];

    /* Setup: all channels with tone period=50, volume=15 */
    ay_init(&ay, 1000000);
    ay_write_reg(&ay, 0, 50);   /* Chan A period */
    ay_write_reg(&ay, 2, 50);   /* Chan B period */
    ay_write_reg(&ay, 4, 50);   /* Chan C period */
    ay_write_reg(&ay, 8, 15);   /* Chan A vol = max */
    ay_write_reg(&ay, 9, 15);   /* Chan B vol = max */
    ay_write_reg(&ay, 10, 15);  /* Chan C vol = max */

    /* Mixer: all tone AND noise disabled (0x3F) → silence */
    ay_write_reg(&ay, 7, 0x3F);
    memset(buf, 0xAA, sizeof(buf));
    ay_generate(&ay, buf, 256);

    /* When both tone and noise disabled, output is HIGH (per AY spec:
     * (tone_out|tone_dis) & (noise_out|noise_dis) = 1|1 & ?|1 = 1).
     * So with volume=15 we get a DC output, not silence.
     * Verify consistent output (all samples same). */
    int all_same = 1;
    int16_t first = buf[0];
    for (int i = 1; i < 512; i++) {
        if (buf[i] != first) { all_same = 0; break; }
    }
    ASSERT_TRUE(all_same);

    /* Mixer: only channel A tone enabled (bit 0=0, rest=1) */
    ay_init(&ay, 1000000);
    ay_write_reg(&ay, 0, 50);
    ay_write_reg(&ay, 7, 0x3E);  /* Only chan A tone enabled */
    ay_write_reg(&ay, 8, 15);    /* Chan A vol = max */
    ay_write_reg(&ay, 9, 0);     /* Chan B vol = 0 (no contribution) */
    ay_write_reg(&ay, 10, 0);    /* Chan C vol = 0 */
    memset(buf, 0, sizeof(buf));
    ay_generate(&ay, buf, 256);

    /* Should have non-zero samples from channel A */
    int has_nonzero = 0;
    for (int i = 0; i < 512; i++) {
        if (buf[i] != 0) { has_nonzero = 1; break; }
    }
    ASSERT_TRUE(has_nonzero);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TIMESTAMPED WRITES (digidrums / sample-accurate audio)            */
/* ═══════════════════════════════════════════════════════════════════ */

static void ay_write_reg_timed(ay3891x_t* ay, uint8_t reg, uint8_t val, uint64_t cyc) {
    ay_write_address(ay, reg);
    ay_write_data_timed(ay, val, cyc);
}

TEST(test_ay_timed_mode_flag) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);
    ASSERT_FALSE(ay.timed_mode);
    ay_write_reg_timed(&ay, 8, 15, 100);  /* a sound-register write */
    ASSERT_TRUE(ay.timed_mode);
    /* Authoritative register updated immediately for reads/keyboard/savestate */
    ASSERT_EQ(ay.registers[8], 15);
}

TEST(test_ay_timed_port_not_queued) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);
    /* Writing an I/O port (reg 14) must update the register immediately and
     * must NOT engage timed mode (no sound effect, nothing queued). */
    ay_write_reg_timed(&ay, 14, 0x5A, 100);
    ASSERT_EQ(ay.registers[14], 0x5A);
    ASSERT_FALSE(ay.timed_mode);
}

TEST(test_ay_digidrum_subbuffer_timing) {
    ay3891x_t ay;
    int16_t buf[256];
    ay_init(&ay, 1000000);

    /* Disable tone+noise on every channel (mixer 0x3F) so the output is a pure
     * DC level set by the channel A volume — ideal to observe a volume change
     * mid-buffer. Only channel A carries volume (B/C stay 0). */
    ay_write_reg_timed(&ay, 7, 0x3F, 0);   /* mixer: all tone+noise disabled */
    ay_write_reg_timed(&ay, 8, 15, 0);     /* chan A volume = max */
    ay_write_reg_timed(&ay, 8, 0, 500);    /* chan A volume -> 0 at cycle 500 */
    ay_write_reg_timed(&ay, 9, 0, 1000);   /* dummy: extend span_end to cycle 1000 */

    /* Span = [0,1000], 100 samples → the volume drop at cycle 500 lands at
     * sample 50 (500*100/1000). Samples 0..49 loud, 50..99 silent. */
    memset(buf, 0x7F, sizeof(buf));
    ay_generate(&ay, buf, 100);

    int16_t loud = buf[0];
    ASSERT_TRUE(loud != 0);          /* first region is the DC high level */
    ASSERT_EQ(buf[49 * 2], loud);    /* still loud just before the transition */
    ASSERT_EQ(buf[50 * 2], 0);       /* silent from sample 50 on */
    ASSERT_EQ(buf[99 * 2], 0);
}

TEST(test_ay_resync_clears_queue) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);
    ay_write_reg_timed(&ay, 8, 15, 100);
    /* A pending event sits in the queue (head != tail). */
    ASSERT_TRUE(ay.evq_head != ay.evq_tail);
    ay_sound_resync(&ay);
    /* Resync drains the queue and mirrors authoritative state into playback. */
    ASSERT_EQ((unsigned)ay.evq_head, (unsigned)ay.evq_tail);
    ASSERT_EQ(ay.play.sregs[8], 15);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  AY-3-8910 PSG Audio Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_ay_init);
    RUN(test_ay_write_read);
    RUN(test_ay_envelope_shape0);
    RUN(test_ay_envelope_shape8);
    RUN(test_ay_envelope_hold);
    RUN(test_ay_generate_silence);
    RUN(test_ay_generate_tone);
    RUN(test_ay_mixer);
    RUN(test_ay_timed_mode_flag);
    RUN(test_ay_timed_port_not_queued);
    RUN(test_ay_digidrum_subbuffer_timing);
    RUN(test_ay_resync_clears_queue);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
