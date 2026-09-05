/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_sp0256.c
 * @brief GI SP0256-AL2 (Mageco "Synthétiseur Vocal") unit tests
 * @author bmarty <bmarty@mailo.com>
 *
 * The register/timing tests use an all-zero dummy ROM (deterministic, no
 * copyrighted data): every allophone entry decodes to RTS→HALT, so a written
 * command is accepted and immediately completes — exercising the ALD/LRQ/SBY
 * plumbing and the CPU-cycle pacing without shipping the GI mask ROM.
 *
 * An optional synthesis test that actually produces audio runs only when the
 * SP0256_ROM environment variable points at a 2 KB sp0256-al2.bin dump (kept
 * out of the repo). It is skipped — and the suite still passes — when unset.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/sp0256.h"
#include "audio/audio.h"   /* AUDIO_SAMPLE_RATE */

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

/* helper without early-return semantics of ASSERT (used inside setup) */
#define ASSERT_TRUE_NR(x) do { if (!(x)) { printf("FAIL(setup)\n"); tests_failed++; } } while(0)

/* Fill a device with an all-zero (RTS/HALT) dummy ROM. */
static void setup_dummy(sp0256_t* sp, uint16_t addr) {
    static uint8_t zero_rom[SP0256_ROM_SIZE];
    memset(zero_rom, 0, sizeof(zero_rom));
    sp0256_init(sp, addr);
    ASSERT_TRUE_NR(sp0256_load_rom(sp, zero_rom, SP0256_ROM_SIZE));
}

/* ── init / reset defaults ─────────────────────────────────────────────── */
TEST(test_init_defaults) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    ASSERT_EQ(sp.base_addr, 0x03F1);
    ASSERT_FALSE(sp.rom_valid);
    ASSERT_EQ(sp.lrq, 1);          /* ready for a command */
    ASSERT_EQ(sp.sby, 1);          /* standby (idle)      */
    ASSERT_EQ(sp.halted, 1);
    ASSERT_FALSE(sp0256_speaking(&sp));
}

TEST(test_init_default_addr_when_zero) {
    sp0256_t sp;
    sp0256_init(&sp, 0);
    ASSERT_EQ(sp.base_addr, SP0256_BASE_DEFAULT);
}

/* ── ROM loading ───────────────────────────────────────────────────────── */
TEST(test_load_rom_wrong_size) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    uint8_t buf[100] = {0};
    ASSERT_FALSE(sp0256_load_rom(&sp, buf, 100));
    ASSERT_FALSE(sp.rom_valid);
}

TEST(test_load_rom_ok) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    uint8_t buf[SP0256_ROM_SIZE] = {0};
    ASSERT_TRUE(sp0256_load_rom(&sp, buf, SP0256_ROM_SIZE));
    ASSERT_TRUE(sp.rom_valid);
}

/* ── status register ───────────────────────────────────────────────────── */
TEST(test_status_idle_bits) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    uint8_t s = sp0256_read(&sp, 0x03F1);
    ASSERT_TRUE(s & SP0256_STAT_LRQ);   /* ready  */
    ASSERT_TRUE(s & SP0256_STAT_SBY);   /* idle   */
}

TEST(test_read_wrong_addr) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    ASSERT_EQ(sp0256_read(&sp, 0x0300), 0xFF);
}

/* ── ALD write handshake ───────────────────────────────────────────────── */
TEST(test_write_ald_sets_busy) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    sp0256_write(&sp, 0x03F1, 0x18);    /* allophone 0x18 (@AA) */
    ASSERT_EQ(sp.lrq, 0);               /* now busy */
    ASSERT_EQ(sp.sby, 0);               /* speaking */
    ASSERT_EQ(sp.ald, 0x18 << 4);       /* 2-byte jump-table entry */
}

TEST(test_write_masks_to_6_bits) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    sp0256_write(&sp, 0x03F1, 0xC5);    /* high bits ignored → allophone 5 */
    ASSERT_EQ(sp.ald, (0xC5 & 0x3F) << 4);
}

TEST(test_write_dropped_when_busy) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    sp0256_write(&sp, 0x03F1, 0x10);    /* accepted */
    int ald_after_first = sp.ald;
    sp0256_write(&sp, 0x03F1, 0x20);    /* busy → dropped */
    ASSERT_EQ(sp.ald, ald_after_first);
}

TEST(test_write_wrong_addr_ignored) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    sp0256_write(&sp, 0x0300, 0x10);
    ASSERT_EQ(sp.lrq, 1);               /* unchanged */
}

/* ── CPU-cycle pacing / microsequencer plumbing (dummy zero ROM) ───────── */
TEST(test_tick_processes_and_returns_to_standby) {
    sp0256_t sp;
    setup_dummy(&sp, 0x03F1);

    sp0256_write(&sp, 0x03F1, 0x05);
    ASSERT_EQ(sp.sby, 0);               /* command pending */

    /* A zero ROM decodes to RTS→HALT: after a few samples the chip is idle. */
    for (int i = 0; i < 8; i++)
        sp0256_tick(&sp, 1000);         /* 10 samples per tick @10 kHz */

    ASSERT_EQ(sp.lrq, 1);               /* ready again */
    ASSERT_EQ(sp.sby, 1);               /* back to standby */
    ASSERT_FALSE(sp0256_speaking(&sp));
}

TEST(test_tick_noop_without_rom) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);           /* no ROM loaded */
    sp0256_tick(&sp, 100000);           /* must not crash / advance */
    ASSERT_FALSE(sp.rom_valid);
}

/* ── audio generation ──────────────────────────────────────────────────── */
TEST(test_generate_silence_without_rom) {
    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    int16_t out[256];
    memset(out, 0x7F, sizeof(out));
    sp0256_generate(&sp, out, 256);
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(out[i], 0);
}

TEST(test_generate_silence_when_idle) {
    sp0256_t sp;
    setup_dummy(&sp, 0x03F1);
    int16_t out[882];
    sp0256_tick(&sp, 19968);            /* one frame, idle */
    sp0256_generate(&sp, out, 882);
    int nonzero = 0;
    for (int i = 0; i < 882; i++) if (out[i]) nonzero++;
    ASSERT_EQ(nonzero, 0);              /* idle chip is silent */
}

/* ── optional: real synthesis with sp0256-al2.bin (SP0256_ROM env) ─────── */
TEST(test_real_rom_speaks) {
    const char* path = getenv("SP0256_ROM");
    if (!path) {
        printf("SKIP (set SP0256_ROM=al2.bin to enable)  ");
        return;
    }
    FILE* f = fopen(path, "rb");
    if (!f) { printf("SKIP (cannot open %s)  ", path); return; }
    uint8_t rom[SP0256_ROM_SIZE];
    size_t rd = fread(rom, 1, SP0256_ROM_SIZE, f);
    fclose(f);
    ASSERT_EQ(rd, (size_t)SP0256_ROM_SIZE);

    sp0256_t sp;
    sp0256_init(&sp, 0x03F1);
    ASSERT_TRUE(sp0256_load_rom(&sp, rom, SP0256_ROM_SIZE));

    sp0256_write(&sp, 0x03F1, 0x18);    /* @AA — a voiced vowel */
    ASSERT_TRUE(sp0256_speaking(&sp));

    int16_t out[882];
    int max_abs = 0, was_speaking = 0, finished_frame = -1;
    for (int frame = 0; frame < 60; frame++) {   /* up to ~1.2 s */
        sp0256_tick(&sp, 19968);
        sp0256_generate(&sp, out, 882);
        for (int i = 0; i < 882; i++) {
            int a = out[i] < 0 ? -out[i] : out[i];
            if (a > max_abs) max_abs = a;
        }
        if (sp0256_speaking(&sp)) was_speaking = 1;
        if (!sp0256_speaking(&sp) && frame > 0) { finished_frame = frame; break; }
    }
    ASSERT_TRUE(was_speaking);
    ASSERT_TRUE(max_abs > 100);         /* produced audible output */
    /* Correct microcode decoding ⇒ the allophone TERMINATES (RTS→HALT) at a
     * realistic duration; a wrong ROM bit order would run forever (never idle). */
    ASSERT_TRUE(finished_frame > 0);
    ASSERT_TRUE(finished_frame < 55);
    printf("(peak=%d, %d frames)  ", max_abs, finished_frame);
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  SP0256-AL2 (Mageco speech synth) tests\n");
    printf("═══════════════════════════════════════════════════════\n");

    RUN(test_init_defaults);
    RUN(test_init_default_addr_when_zero);
    RUN(test_load_rom_wrong_size);
    RUN(test_load_rom_ok);
    RUN(test_status_idle_bits);
    RUN(test_read_wrong_addr);
    RUN(test_write_ald_sets_busy);
    RUN(test_write_masks_to_6_bits);
    RUN(test_write_dropped_when_busy);
    RUN(test_write_wrong_addr_ignored);
    RUN(test_tick_processes_and_returns_to_standby);
    RUN(test_tick_noop_without_rom);
    RUN(test_generate_silence_without_rom);
    RUN(test_generate_silence_when_idle);
    RUN(test_real_rom_speaks);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
