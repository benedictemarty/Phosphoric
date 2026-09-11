/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_audio.c
 * @brief AY-3-8910 PSG audio unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.0.0-rc
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
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
    /* Ce test observe le GÉNÉRATEUR, qui produit ici un niveau continu. L'étage
     * de sortie de l'ORIC (couplage capacitif) le supprimerait : on le désarme
     * pour mesurer la source, pas la chaîne. */
    ay.dc_block_off = true;
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
    ay.dc_block_off = true;
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
    ay.dc_block_off = true;                /* on mesure le générateur, pas l'étage de sortie */
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

/* The noise LFSR must advance at clock/(16*NP) — half the tone toggle rate
 * (datasheet: same /16 prescaler as the tone, but no square-wave ÷2). We
 * generate a known number of samples and compare the LFSR against a reference
 * advanced by exactly the number of steps the clock/16 rate predicts. With the
 * old (buggy) clock/8 rate the LFSR would have advanced ~twice as far. */
TEST(test_ay_noise_rate_clock_div16) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);
    const uint8_t np = 5;
    ay_write_reg(&ay, 6, np);            /* noise period */
    ay_write_reg(&ay, 7, 0x07);          /* tone off (bits0-2), noise on chans A/B/C */

    const int K = 40;                    /* samples to render */
    int16_t buf[40 * 2];
    ay_generate(&ay, buf, K);

    /* Steps taken = floor(noise_rate * K / (NP * SAMPLE_RATE)), noise_rate = clock/16. */
    uint64_t noise_rate = ay.clock_rate / 16;
    uint64_t steps = noise_rate * (uint64_t)K / ((uint64_t)np * AUDIO_SAMPLE_RATE);
    ASSERT_TRUE(steps > 0);

    /* Reference 17-bit LFSR (taps bit0 ^ bit3) advanced `steps` times from seed 1. */
    uint32_t ref = 1;
    for (uint64_t s = 0; s < steps; s++) {
        uint32_t bit = ((ref >> 0) ^ (ref >> 3)) & 1;
        ref = (ref >> 1) | (bit << 16);
    }
    ASSERT_EQ(ay.noise_shift, ref);

    /* Sanity: the buggy clock/8 rate would have advanced ~2x as many steps,
     * yielding a different LFSR state. */
    uint32_t ref_fast = 1;
    for (uint64_t s = 0; s < steps * 2; s++) {
        uint32_t bit = ((ref_fast >> 0) ^ (ref_fast >> 3)) & 1;
        ref_fast = (ref_fast >> 1) | (bit << 16);
    }
    ASSERT_TRUE(ay.noise_shift != ref_fast);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════ */
/*  CADENCEMENT MATÉRIEL DU PSG (V2-E5)                                */
/*                                                                    */
/*  Le PSG tourne à clock/8 (125 kHz sur l'ORIC), pas au taux          */
/*  d'échantillonnage. Ces tests mesurent le SIGNAL produit et le      */
/*  comparent aux formules de la datasheet, plutôt que de comparer des */
/*  octets à une référence figée : une comparaison spectrale reste     */
/*  vraie même si le rendu évolue.                                     */
/* ═══════════════════════════════════════════════════════════════════ */

#define SPEC_SAMPLES 44100
static int16_t spec_buf[SPEC_SAMPLES * 2];

/* Fréquence d'un signal, par comptage des passages par sa valeur moyenne. */
static double measure_tone_hz(int period) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);
    ay.dc_block_off = true;                /* propriété du générateur */
    ay_write_reg(&ay, 0, period & 0xFF);
    ay_write_reg(&ay, 1, (period >> 8) & 0x0F);
    ay_write_reg(&ay, 7, 0x3E);            /* canal A : ton seul */
    ay_write_reg(&ay, 8, 15);              /* volume maximal */
    ay_generate(&ay, spec_buf, SPEC_SAMPLES);

    long sum = 0;
    for (int i = 0; i < SPEC_SAMPLES; i++) sum += spec_buf[i * 2];
    double mean = (double)sum / SPEC_SAMPLES;
    int crossings = 0;
    for (int i = 1; i < SPEC_SAMPLES; i++)
        if ((spec_buf[(i - 1) * 2] > mean) != (spec_buf[i * 2] > mean)) crossings++;
    return crossings / 2.0;                /* une période = deux passages */
}

/* Énergie du signal autour de sa moyenne. */
static double measure_rms(int period) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);
    ay.dc_block_off = true;
    ay_write_reg(&ay, 0, period & 0xFF);
    ay_write_reg(&ay, 1, (period >> 8) & 0x0F);
    ay_write_reg(&ay, 7, 0x3E);
    ay_write_reg(&ay, 8, 15);
    ay_generate(&ay, spec_buf, SPEC_SAMPLES);

    double mean = 0;
    for (int i = 0; i < SPEC_SAMPLES; i++) mean += spec_buf[i * 2];
    mean /= SPEC_SAMPLES;
    double s2 = 0;
    for (int i = 0; i < SPEC_SAMPLES; i++) {
        double d = spec_buf[i * 2] - mean;
        s2 += d * d;
    }
    return sqrt(s2 / SPEC_SAMPLES);
}

/* La fréquence d'un ton doit valoir clock/(16·P) — la formule de la datasheet. */
TEST(test_ay_tone_frequency_matches_datasheet) {
    static const int periods[] = { 4, 8, 16, 50, 100, 284, 500 };
    for (unsigned i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
        double theory = 1000000.0 / (16.0 * periods[i]);
        double measured = measure_tone_hz(periods[i]);
        /* Tolérance : 0,5 % ou 1 Hz (résolution du comptage sur une seconde). */
        double tol = theory * 0.005;
        if (tol < 1.0) tol = 1.0;
        if (fabs(measured - theory) > tol) {
            printf("FAIL\n    P=%d: attendu %.2f Hz, mesuré %.2f Hz\n",
                   periods[i], theory, measured);
            tests_failed++;
            return;
        }
    }
}

/* L'enveloppe avance d'un pas tous les EP pas d'horloge interne, soit
 * clock/(8·EP) : un cycle de 32 pas dure donc 256·EP/clock, la formule de la
 * datasheet. La forme $00 décroît de 15 à 0 en 15 pas puis se tait : on mesure
 * l'instant d'extinction. Avant la V2, l'enveloppe était DEUX FOIS trop lente. */
TEST(test_ay_envelope_period_matches_datasheet) {
    static const int eps[] = { 100, 200, 500, 1000 };
    for (unsigned i = 0; i < sizeof(eps) / sizeof(eps[0]); i++) {
        ay3891x_t ay;
        ay_init(&ay, 1000000);
        ay.dc_block_off = true;            /* l'enveloppe est une propriété du générateur */
        ay_write_reg(&ay, 7, 0x3F);        /* ton et bruit coupés */
        ay_write_reg(&ay, 8, 0x10);        /* volume piloté par l'enveloppe */
        ay_write_reg(&ay, 11, eps[i] & 0xFF);
        ay_write_reg(&ay, 12, (eps[i] >> 8) & 0xFF);
        ay_write_reg(&ay, 13, 0x00);       /* décroissance simple puis silence */
        ay_generate(&ay, spec_buf, SPEC_SAMPLES);

        int silent_at = -1;
        for (int k = 0; k < SPEC_SAMPLES; k++)
            if (spec_buf[k * 2] == 0) { silent_at = k; break; }
        ASSERT_TRUE(silent_at > 0);

        double measured = silent_at / (double)AUDIO_SAMPLE_RATE;
        double theory = 15.0 * 8.0 * eps[i] / 1000000.0;   /* 15 pas */
        if (fabs(measured - theory) > theory * 0.02) {
            printf("FAIL\n    EP=%d: attendu %.5f s, mesuré %.5f s\n",
                   eps[i], theory, measured);
            tests_failed++;
            return;
        }
    }
}

/* Au-delà de Nyquist, la sortie doit s'ATTÉNUER, pas se replier en bruit : le
 * PSG étant cadencé au matériel, chaque échantillon intègre les transitions
 * qu'il couvre (filtre boîte). P=1 vaut 62,5 kHz, bien au-dessus des 22 kHz
 * représentables — son énergie doit s'effondrer. */
TEST(test_ay_no_aliasing_above_nyquist) {
    double rms_audible = measure_rms(50);     /* 1250 Hz */
    double rms_ultra   = measure_rms(1);      /* 62,5 kHz */
    ASSERT_TRUE(rms_audible > 2000.0);
    ASSERT_TRUE(rms_ultra < rms_audible / 2.0);
}

/* Le générateur de bruit est un LFSR 17 bits (x^17 + x^14 + 1). Deux propriétés
 * vérifiables sans dépendre de la phase d'échantillonnage : il ne se bloque
 * jamais sur l'état zéro (un LFSR qui y tombe reste muet à jamais), et sa sortie
 * est équilibrée — un bruit blanc, pas un motif. La conformité de la SÉQUENCE
 * est vérifiée séparément par test_ay_noise_rate_clock_div16, qui la compare pas
 * à pas à un LFSR de référence. */
TEST(test_ay_noise_lfsr_is_healthy) {
    ay3891x_t ay;
    ay_init(&ay, 1000000);
    ay.dc_block_off = true;
    ay_write_reg(&ay, 6, 1);               /* période de bruit minimale */
    ay_write_reg(&ay, 7, 0x07);            /* bruit sur les trois canaux */
    ASSERT_EQ(ay.noise_shift, 1u);

    int16_t one[2];
    long ones = 0;
    const long total = 200000;
    for (long k = 0; k < total; k++) {
        ay_generate(&ay, one, 1);
        ASSERT_TRUE(ay.noise_shift != 0u);   /* jamais bloqué à zéro */
        ones += ay.noise_output ? 1 : 0;
    }
    /* Bruit équilibré : la proportion de 1 doit rester proche de la moitié. */
    double ratio = (double)ones / (double)total;
    ASSERT_TRUE(ratio > 0.45 && ratio < 0.55);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ÉTAGE DE SORTIE (V2-E5) — d'après le schéma officiel Oric-1/Atmos   */
/*                                                                    */
/*  Les trois sorties CH_A/CH_B/CH_C sont reliées ensemble sur une     */
/*  charge commune (R4 = 1 kΩ) : le mixage parallèle MOYENNE les       */
/*  tensions au lieu de les additionner. Puis un condensateur de       */
/*  couplage (C4) rejoint l'ampli LM386 : il bloque le continu.        */
/* ═══════════════════════════════════════════════════════════════════ */

/* Mesure (moyenne, min, max, RMS) sur la seconde moitié du rendu, une fois le
 * blocage de continu convergé (sa constante de temps vaut 4096 échantillons). */
static void measure_output(ay3891x_t* ay, double* mean, double* mn, double* mx,
                           double* rms) {
    ay_generate(ay, spec_buf, SPEC_SAMPLES);
    int from = SPEC_SAMPLES / 2, n = SPEC_SAMPLES - from;
    double m = 0, lo = 32767, hi = -32768;
    for (int i = from; i < SPEC_SAMPLES; i++) {
        double v = spec_buf[i * 2];
        m += v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    m /= n;
    double s2 = 0;
    for (int i = from; i < SPEC_SAMPLES; i++) {
        double d = spec_buf[i * 2] - m;
        s2 += d * d;
    }
    if (mean) *mean = m;
    if (mn) *mn = lo;
    if (mx) *mx = hi;
    if (rms) *rms = sqrt(s2 / n);
}

static void setup_three_tones(ay3891x_t* ay, int nch, int vol) {
    ay_init(ay, 1000000);
    ay_write_reg(ay, 0, 100);              /* trois périodes distinctes */
    ay_write_reg(ay, 2, 120);
    ay_write_reg(ay, 4, 150);
    uint8_t mixer = 0x3F;                  /* tout coupé */
    for (int c = 0; c < nch; c++) mixer &= (uint8_t)~(1 << c);
    ay_write_reg(ay, 7, mixer);
    for (int c = 0; c < 3; c++) ay_write_reg(ay, 8 + c, c < nch ? vol : 0);
}

/* La sortie ne doit pas porter de composante continue : sur la machine, le
 * condensateur de couplage C4 la bloque avant l'ampli. Sans ce blocage, le
 * signal du PSG est unipolaire (0 → +max) et son continu vaut la moitié de son
 * amplitude — un décalage qu'aucun haut-parleur ne restitue. */
TEST(test_ay_output_has_no_dc_offset) {
    for (int nch = 1; nch <= 3; nch++) {
        ay3891x_t ay;
        setup_three_tones(&ay, nch, 15);
        double mean, mn, mx, rms;
        measure_output(&ay, &mean, &mn, &mx, &rms);
        /* Continu résiduel négligeable devant l'amplitude du signal. */
        ASSERT_TRUE(fabs(mean) < rms * 0.02);
        /* …et signal centré : les excursions sont symétriques à 5 % près. */
        ASSERT_TRUE(fabs(mx + mn) < (mx - mn) * 0.05);
    }
}

/* Le blocage du continu ne doit rien retirer d'audible : coupant à ~1,7 Hz, il
 * laisse le contenu intact. On compare l'énergie avec et sans. */
TEST(test_ay_dc_block_preserves_audio) {
    ay3891x_t a, b;
    setup_three_tones(&a, 3, 15);
    setup_three_tones(&b, 3, 15);
    b.dc_block_off = true;                 /* sortie brute, pour comparaison */

    double rms_filtered, rms_raw;
    measure_output(&a, NULL, NULL, NULL, &rms_filtered);
    measure_output(&b, NULL, NULL, NULL, &rms_raw);
    ASSERT_TRUE(fabs(rms_filtered - rms_raw) < rms_raw * 0.02);
}

/* Mixage parallèle : sur le PCB de l'ORIC, les trois sorties sont reliées à une
 * charge commune, ce qui MOYENNE les tensions au lieu de les sommer — un canal
 * seul à 1 V donne ≈ 0,33 V, mesure rapportée sur le forum Defence Force. La
 * dynamique crête à crête de trois canaux doit donc valoir trois fois celle d'un
 * seul, et non rester identique. */
TEST(test_ay_parallel_mixing_averages_channels) {
    double pp[4];
    for (int nch = 1; nch <= 3; nch++) {
        ay3891x_t ay;
        setup_three_tones(&ay, nch, 15);
        double mn, mx;
        measure_output(&ay, NULL, &mn, &mx, NULL);
        pp[nch] = mx - mn;
    }
    ASSERT_TRUE(pp[1] > 0);
    /* rapports 2/1 et 3/1 à 5 % près */
    ASSERT_TRUE(fabs(pp[2] / pp[1] - 2.0) < 0.05);
    ASSERT_TRUE(fabs(pp[3] / pp[1] - 3.0) < 0.05);
}

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
    RUN(test_ay_noise_rate_clock_div16);
    RUN(test_ay_timed_mode_flag);
    RUN(test_ay_timed_port_not_queued);
    RUN(test_ay_digidrum_subbuffer_timing);
    RUN(test_ay_resync_clears_queue);

    printf("\n  Cadencement matériel du PSG (V2-E5):\n");
    RUN(test_ay_tone_frequency_matches_datasheet);
    RUN(test_ay_envelope_period_matches_datasheet);
    RUN(test_ay_no_aliasing_above_nyquist);
    RUN(test_ay_noise_lfsr_is_healthy);

    printf("\n  Étage de sortie, d'après le schéma Oric (V2-E5):\n");
    RUN(test_ay_output_has_no_dc_offset);
    RUN(test_ay_dc_block_preserves_audio);
    RUN(test_ay_parallel_mixing_averages_channels);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
