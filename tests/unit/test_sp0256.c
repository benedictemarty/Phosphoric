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

/* Reads the section back into a memory buffer (byte-by-byte comparison). */
static long section_bytes(FILE* fp, unsigned char* buf, long cap) {
    long n = ftell(fp);
    if (n > cap) return -1;
    rewind(fp);
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) return -1;
    return n;
}

/* ── Section .ost « SPO » (sprint D) ─────────────────────────────────────── */

/* Pseudo-random ROM with a fixed seed: the microsequencer runs arbitrary but
 * deterministic "code" from it, which exercises the whole state (sequencer,
 * filter, buffer) without the real ROM (not redistributable). */
static void fill_prng_rom(uint8_t* rom) {
    uint32_t x = 0x12345678u;
    for (int i = 0; i < SP0256_ROM_SIZE; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rom[i] = (uint8_t)x;
    }
}

TEST(test_save_load_resumes_identically) {
    static uint8_t rom[SP0256_ROM_SIZE];
    fill_prng_rom(rom);
    sp0256_t* a = calloc(1, sizeof(*a));
    sp0256_t* b = calloc(1, sizeof(*b));
    ASSERT_TRUE(a && b);
    sp0256_init(a, 0x03F1); sp0256_load_rom(a, rom, SP0256_ROM_SIZE);
    sp0256_write(a, 0x03F1, 0x18);
    int16_t oa[882], ob[882];
    sp0256_tick(a, 19968);
    sp0256_generate(a, oa, 441);              /* leaves samples pending */

    FILE* fp = tmpfile();
    ASSERT_TRUE(fp != NULL);
    ASSERT_TRUE(sp0256_save(a, fp));
    long size = ftell(fp);
    rewind(fp);
    sp0256_init(b, 0x03F1); sp0256_load_rom(b, rom, SP0256_ROM_SIZE);
    sp0256_load(b, fp, (uint32_t)size);
    /* Round-trip identity: saving the reloaded copy again yields the same
     * bytes — any field forgotten on load shows up here. */
    static unsigned char s1[40000], s2[40000];
    long n1 = section_bytes(fp, s1, sizeof(s1));
    FILE* fp2 = tmpfile();
    ASSERT_TRUE(fp2 != NULL);
    sp0256_save(b, fp2);
    long n2 = section_bytes(fp2, s2, sizeof(s2));
    fclose(fp2);
    fclose(fp);
    ASSERT_TRUE(n1 > 0);
    ASSERT_EQ(n2, n1);
    ASSERT_EQ(memcmp(s1, s2, (size_t)n1), 0);

    ASSERT_EQ(b->pc, a->pc);
    ASSERT_EQ(b->sc_head - b->sc_tail, a->sc_head - a->sc_tail);
    int differ = 0;
    for (int f = 0; f < 10; f++) {
        sp0256_tick(a, 19968); sp0256_tick(b, 19968);
        sp0256_generate(a, oa, 882); sp0256_generate(b, ob, 882);
        if (memcmp(oa, ob, sizeof(oa)) != 0) differ++;
        if (a->pc != b->pc || a->lrq != b->lrq || a->sby != b->sby) differ++;
    }
    ASSERT_EQ(differ, 0);
    free(a); free(b);
}

/* Unknown version or unexpected size: the current state is left untouched. */
TEST(test_load_rejects_bad_section) {
    static uint8_t rom[SP0256_ROM_SIZE];
    fill_prng_rom(rom);
    sp0256_t* a = calloc(1, sizeof(*a));
    sp0256_t* b = calloc(1, sizeof(*b));
    ASSERT_TRUE(a && b);
    sp0256_init(a, 0x03F1); sp0256_load_rom(a, rom, SP0256_ROM_SIZE);
    sp0256_write(a, 0x03F1, 0x18);
    sp0256_tick(a, 19968);
    FILE* fp = tmpfile();
    ASSERT_TRUE(fp != NULL);
    sp0256_save(a, fp);
    long size = ftell(fp);

    sp0256_init(b, 0x03F1);
    rewind(fp);
    sp0256_load(b, fp, (uint32_t)size + 1);             /* wrong size */
    ASSERT_EQ(b->halted, 1);
    rewind(fp); fputc(SP0256_SAVE_VERSION + 1, fp);     /* unknown version */
    rewind(fp);
    sp0256_load(b, fp, (uint32_t)size);
    ASSERT_EQ(b->halted, 1);
    ASSERT_EQ(b->sby, 1);
    fclose(fp);
    free(a); free(b);
}

/* Every saved field is restored: source state filled with a pattern (except
 * fields clamped on load), fresh copy reloaded then saved again → same
 * bytes. A forgotten field would keep its default value and diverge. */
TEST(test_load_restores_every_field) {
    sp0256_t* a = calloc(1, sizeof(*a));
    sp0256_t* b = calloc(1, sizeof(*b));
    ASSERT_TRUE(a && b);
    memset(a, 0x5A, sizeof(*a));
    a->emu = NULL;
    FILE* f1 = tmpfile(); FILE* f2 = tmpfile();
    ASSERT_TRUE(f1 && f2);
    sp0256_save(a, f1);
    long n1 = ftell(f1);
    rewind(f1);
    sp0256_init(b, 0);
    sp0256_load(b, f1, (uint32_t)n1);
    sp0256_save(b, f2);
    static unsigned char s1[40000], s2[40000];
    long m1 = section_bytes(f1, s1, sizeof(s1));
    long m2 = section_bytes(f2, s2, sizeof(s2));
    fclose(f1); fclose(f2);
    ASSERT_TRUE(m1 > 0);
    ASSERT_EQ(m2, m1);
    long first = -1;
    for (long i = 0; i < m1; i++) if (s1[i] != s2[i]) { first = i; break; }
    if (first >= 0) printf("(1er écart à l'octet %ld)  ", first);
    ASSERT_EQ(first, -1);
    free(a); free(b);
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
    RUN(test_save_load_resumes_identically);
    RUN(test_load_rejects_bad_section);
    RUN(test_load_restores_every_field);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
