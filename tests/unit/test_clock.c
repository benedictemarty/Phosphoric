/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_clock.c
 * @brief Master clock: one machine cycle, intra-cycle order (V2-E2)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * Checks the contract of `emu_cycle()` (src/emu_clock.c):
 *   - one call = exactly one CPU cycle, and the scan position advances with it;
 *   - the intra-cycle order **φ1 ULA → φ2 CPU**: a CPU write during
 *     cycle *c* is NOT visible to the scanline emitted at that same cycle; it only
 *     becomes visible from the next cycle on (this is the hardware visibility
 *     convention, where the ULA accesses RAM in φ1);
 *   - the φ2 peripherals do receive **one** cycle at a time (no batching);
 *   - the PAL frame has 312 lines of 64 cycles and 224 visible lines.
 */

#include "emulator.h"
#include "cpu/microseq.h"
#include "video/video.h"
#include "io/ula_ng.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-52s", #name); name(); tests_passed++; printf("PASS\n"); } while (0)
#define ASSERT_EQ(a, b) do { \
    if ((long long)(a) != (long long)(b)) { \
        printf("FAIL\n    %s:%d: attendu %lld, obtenu %lld\n", __FILE__, __LINE__, \
               (long long)(b), (long long)(a)); \
        tests_failed++; return; \
    } \
} while (0)
#define ASSERT_TRUE(x) do { \
    if (!(x)) { printf("FAIL\n    %s:%d: attendu vrai\n", __FILE__, __LINE__); \
                tests_failed++; return; } \
} while (0)

/* A minimal emulator: memory, CPU, video, VIA, ULA-NG. No storage
 * or sound peripheral — the clock does not need them. */
static emulator_t g_emu;

/* Clock callback identical in spirit to the one in main.c: here it is used to
 * count the cycles delivered to the φ2 peripherals. */
static int g_tick_calls, g_tick_cycles, g_tick_max;
static void clock_tick(void* ctx, int cycles) {
    emulator_t* emu = (emulator_t*)ctx;
    if (cycles == 1) via_tick(&emu->via);
    else             via_update(&emu->via, cycles);
    g_tick_calls++;
    g_tick_cycles += cycles;
    if (cycles > g_tick_max) g_tick_max = cycles;
}

static void setup(const uint8_t* code, size_t n, bool microseq) {
    memset(&g_emu, 0, sizeof(g_emu));
    memory_init(&g_emu.memory);
    video_init(&g_emu.video);
    via_init(&g_emu.via);
    ula_ng_init(&g_emu.ula_ng);
    cpu_init(&g_emu.cpu, &g_emu.memory);
    cpu_set_microseq(&g_emu.cpu, microseq);
    cpu_set_cycle_callback(&g_emu.cpu, clock_tick, &g_emu);
    g_emu.memory.rom_enabled = false;
    for (size_t i = 0; i < n; i++) g_emu.memory.ram[0x0200 + i] = code[i];
    g_emu.cpu.PC = 0x0200;
    emu_clock_frame_begin(&g_emu);
    g_tick_calls = g_tick_cycles = g_tick_max = 0;
}

/* Counts the non-black pixels of framebuffer line `y`. */
static int line_ink(const video_t* vid, int y) {
    int n = 0;
    for (int x = 0; x < vid->native_w; x++) {
        const uint8_t* p = &vid->framebuffer[(y * vid->native_w + x) * 3];
        if (p[0] || p[1] || p[2]) n++;
    }
    return n;
}

TEST(test_one_call_is_one_cycle) {
    uint8_t code[] = { 0xEA, 0xEA, 0xEA, 0xEA };   /* NOP × 4 */
    setup(code, sizeof(code), true);
    for (int i = 1; i <= 8; i++) {
        emu_cycle(&g_emu);
        ASSERT_EQ((int)g_emu.cpu.cycles, i);       /* one CPU cycle per call */
        ASSERT_EQ(g_emu.raster_cycle, i);          /* the scan follows */
    }
    ASSERT_EQ(g_emu.frame_cycles, 8);
}

TEST(test_peripherals_get_one_cycle_at_a_time) {
    uint8_t code[] = { 0xA9, 0x05, 0x85, 0x10, 0xE6, 0x10,
                       0xA2, 0x03, 0xFE, 0x00, 0x04, 0x48, 0x68 };
    setup(code, sizeof(code), true);
    for (int i = 0; i < 40; i++) emu_cycle(&g_emu);
    ASSERT_EQ(g_tick_cycles, 40);
    ASSERT_EQ(g_tick_calls, 40);     /* one call per cycle… */
    ASSERT_EQ(g_tick_max, 1);        /* …and never batched */
}

TEST(test_raster_position_advances_by_line) {
    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    for (int i = 0; i < PAL_CYCLES_PER_LINE * 3; i++) emu_cycle(&g_emu);
    int line = -1, dot = -1;
    emu_raster_pos(&g_emu, &line, &dot);
    ASSERT_EQ(line, 3);
    ASSERT_EQ(dot, 0);
    ASSERT_EQ(g_emu.raster_rendered, 3);   /* lines 0,1,2 emitted */
}

TEST(test_frame_is_312_lines_of_64_cycles) {
    ASSERT_EQ(PAL_CYCLES_PER_LINE, 64);
    ASSERT_EQ(CYCLES_PER_FRAME, 312 * 64);
    ASSERT_EQ(ULA_NG_FRAME_LINES, 312);

    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    for (int i = 0; i < CYCLES_PER_FRAME; i++) emu_cycle(&g_emu);
    /* Visible area: 224 lines rendered, the rest is blanking. */
    ASSERT_EQ(g_emu.raster_rendered, 224);
    ASSERT_EQ(g_emu.raster_ng_line, 312);
}

TEST(test_frame_begin_and_end) {
    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    for (int i = 0; i < 100; i++) emu_cycle(&g_emu);
    ASSERT_EQ(g_emu.raster_rendered, 1);
    emu_clock_frame_end(&g_emu);           /* finishes the interrupted frame */
    ASSERT_EQ(g_emu.raster_rendered, 224);
    emu_clock_frame_begin(&g_emu);
    ASSERT_EQ(g_emu.raster_cycle, 0);
    ASSERT_EQ(g_emu.raster_rendered, 0);
    ASSERT_EQ(g_emu.raster_ng_line, 0);
}

/* ── The intra-cycle order test (US2.3) ──
 * The CPU write lands exactly on cycle 64, the one at which scanline 0 is
 * emitted. If the ULA does run BEFORE the CPU (φ1 then φ2), line 0 shows
 * the old content; line 1, emitted at cycle 128, shows the new one. */
/* Intra-cycle order MEASURED on hardware (Mike Brown, Unofficial ULA Guide
 * 1.02): the 6502 accesses DRAM first, then the ULA fetches the byte of the same
 * count. A write at cycle c is therefore seen by cell c — and not by
 * cell c-1, fetched at the previous cycle. Up to 2.0.1 the emulator did
 * the opposite and shifted every split one cell to the right. */
TEST(test_cpu_write_visible_in_the_same_cell) {
    /* 5 NOP (10 cycles) then STA $BB8D: the write is the 4th cycle of the STA, i.e.
     * the 14th cycle → count 13 of line 0 → cell 13 ($BB80+13). */
    uint8_t code[] = { 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0x8D, 0x8D, 0xBB };
    setup(code, sizeof(code), true);
    g_emu.ula_per_cycle = true;
    g_emu.ula_fetch_offset = 0;

    memset(&g_emu.memory.ram[0xBB80], 'A', 40 * 28);
    memset(&g_emu.memory.ram[0xB400], 0x3F, 128 * 8);      /* charset: all ink… */
    memset(&g_emu.memory.ram[0xB400 + ' ' * 8], 0x00, 8);  /* …except space, empty */
    g_emu.cpu.A = ' ';

    for (int i = 0; i < PAL_CYCLES_PER_LINE; i++) emu_cycle(&g_emu);
    ASSERT_EQ(g_emu.memory.ram[0xBB8D], ' ');
    ASSERT_EQ(g_emu.raster_rendered, 1);

    int cell12 = 0, cell13 = 0;
    for (int x = 12 * 6; x < 13 * 6; x++) {
        const uint8_t* p = &g_emu.video.framebuffer[x * 3];
        if (p[0] || p[1] || p[2]) cell12++;
    }
    for (int x = 13 * 6; x < 14 * 6; x++) {
        const uint8_t* p = &g_emu.video.framebuffer[x * 3];
        if (p[0] || p[1] || p[2]) cell13++;
    }
    ASSERT_EQ(cell12, 6);   /* fetched at cycle 13, before the write: intact */
    ASSERT_EQ(cell13, 0);   /* fetched at cycle 14, AFTER the CPU write: empty */
}

/* ── Per-cycle ULA: the raster split (V2-E4 / US4.2) ──
 * A CPU write in the middle of a line must affect ONLY the cells
 * not yet fetched. This is the effect that line-by-line rendering made
 * impossible: it sampled the whole line at the same instant. */
TEST(test_ula_per_cycle_mid_line_split) {
    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    g_emu.ula_per_cycle = true;
    g_emu.ula_fetch_offset = 0;

    memset(&g_emu.memory.ram[0xBB80], 'A', 40 * 28);
    memset(&g_emu.memory.ram[0xB400], 0x3F, 128 * 8);   /* full charset… */
    memset(&g_emu.memory.ram[0xB400 + ' ' * 8], 0x00, 8);  /* …except space */

    /* The first 20 cells of line 0 are fetched. */
    for (int i = 0; i < 20; i++) emu_cycle(&g_emu);
    /* The "CPU" clears the whole of line 0 at this instant. */
    memset(&g_emu.memory.ram[0xBB80], ' ', 40);
    /* The rest of the line is scanned. */
    for (int i = 20; i < PAL_CYCLES_PER_LINE; i++) emu_cycle(&g_emu);

    ASSERT_EQ(g_emu.raster_rendered, 1);
    /* Left half: the ink of the 'A' fetched before the write. */
    int left = 0, right = 0;
    for (int x = 0; x < 20 * 6; x++) {
        const uint8_t* p = &g_emu.video.framebuffer[x * 3];
        if (p[0] || p[1] || p[2]) left++;
    }
    for (int x = 20 * 6; x < 40 * 6; x++) {
        const uint8_t* p = &g_emu.video.framebuffer[x * 3];
        if (p[0] || p[1] || p[2]) right++;
    }
    ASSERT_EQ(left, 20 * 6);    /* fetched before the write: full */
    ASSERT_EQ(right, 0);        /* fetched after: empty */
}

/* Without per-cycle mode, the same sequence yields a uniformly empty line:
 * the whole line is sampled at the end, hence after the write. This is the
 * counter-test — and the gap that epic E4 closes. */
TEST(test_line_render_cannot_split) {
    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    g_emu.ula_per_cycle = false;

    memset(&g_emu.memory.ram[0xBB80], 'A', 40 * 28);
    memset(&g_emu.memory.ram[0xB400], 0x3F, 128 * 8);
    memset(&g_emu.memory.ram[0xB400 + ' ' * 8], 0x00, 8);

    for (int i = 0; i < 20; i++) emu_cycle(&g_emu);
    memset(&g_emu.memory.ram[0xBB80], ' ', 40);
    for (int i = 20; i < PAL_CYCLES_PER_LINE; i++) emu_cycle(&g_emu);

    ASSERT_EQ(g_emu.raster_rendered, 1);
    int ink = line_ink(&g_emu.video, 0);
    ASSERT_EQ(ink, 0);          /* the whole line sees the write */
}

/* The fetch offset shifts the column read at a given cycle: with an offset of
 * 10, column 0 is fetched at cycle 10, so the split moves by the same amount. */
TEST(test_ula_fetch_offset_moves_the_split) {
    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    g_emu.ula_per_cycle = true;
    g_emu.ula_fetch_offset = 10;

    memset(&g_emu.memory.ram[0xBB80], 'A', 40 * 28);
    memset(&g_emu.memory.ram[0xB400], 0x3F, 128 * 8);
    memset(&g_emu.memory.ram[0xB400 + ' ' * 8], 0x00, 8);

    for (int i = 0; i < 20; i++) emu_cycle(&g_emu);   /* columns 0..9 fetched */
    memset(&g_emu.memory.ram[0xBB80], ' ', 40);
    for (int i = 20; i < PAL_CYCLES_PER_LINE; i++) emu_cycle(&g_emu);

    int left = 0;
    for (int x = 0; x < 10 * 6; x++) {
        const uint8_t* p = &g_emu.video.framebuffer[x * 3];
        if (p[0] || p[1] || p[2]) left++;
    }
    int right = 0;
    for (int x = 10 * 6; x < 40 * 6; x++) {
        const uint8_t* p = &g_emu.video.framebuffer[x * 3];
        if (p[0] || p[1] || p[2]) right++;
    }
    ASSERT_EQ(left, 10 * 6);
    ASSERT_EQ(right, 0);
}

/* Static screen: both paths must yield EXACTLY the same image.
 * This is the non-regression guarantee of per-cycle mode. */
TEST(test_static_screen_identical_both_paths) {
    uint8_t code[] = { 0xEA };
    static uint8_t fb_line[VIDEO_MAX_W * VIDEO_MAX_H * 3];

    setup(code, sizeof(code), true);
    g_emu.ula_per_cycle = false;
    for (int i = 0; i < 0xBB80; i++) g_emu.memory.ram[i] = 0;
    for (int i = 0; i < 40 * 28; i++) g_emu.memory.ram[0xBB80 + i] = (uint8_t)(' ' + (i % 60));
    memset(&g_emu.memory.ram[0xB400], 0x5A, 128 * 8);
    for (int i = 0; i < CYCLES_PER_FRAME; i++) emu_cycle(&g_emu);
    memcpy(fb_line, g_emu.video.framebuffer, sizeof(fb_line));

    setup(code, sizeof(code), true);
    g_emu.ula_per_cycle = true;
    for (int i = 0; i < 0xBB80; i++) g_emu.memory.ram[i] = 0;
    for (int i = 0; i < 40 * 28; i++) g_emu.memory.ram[0xBB80 + i] = (uint8_t)(' ' + (i % 60));
    memset(&g_emu.memory.ram[0xB400], 0x5A, 128 * 8);
    for (int i = 0; i < CYCLES_PER_FRAME; i++) emu_cycle(&g_emu);

    ASSERT_EQ(memcmp(fb_line, g_emu.video.framebuffer, sizeof(fb_line)), 0);
}

/* The legacy core cannot stop between two cycles: emu_cycle() there
 * executes a whole instruction, and the scan catches up accordingly. */
TEST(test_legacy_core_advances_by_instruction) {
    uint8_t code[] = { 0xA9, 0x42 };   /* LDA #$42 = 2 cycles */
    setup(code, sizeof(code), false);
    bool done = emu_cycle(&g_emu);
    ASSERT_TRUE(done);
    ASSERT_EQ((int)g_emu.cpu.cycles, 2);
    ASSERT_EQ(g_emu.raster_cycle, 2);
    ASSERT_EQ(g_emu.cpu.A, 0x42);
}

TEST(test_emu_step_returns_instruction_cycles) {
    uint8_t code[] = { 0x20, 0x00, 0x03 };   /* JSR $0300 = 6 cycles */
    setup(code, sizeof(code), true);
    int n = emu_step(&g_emu);
    ASSERT_EQ(n, 6);
    ASSERT_EQ((int)g_emu.cpu.cycles, 6);
    ASSERT_EQ(g_emu.raster_cycle, 6);
    ASSERT_EQ(g_emu.cpu.PC, 0x0300);
}

/* V2-E7: the "one call = one cycle" contract holds for ALL instructions,
 * including those whose length is decided along the way. A branch
 * not taken (2 cycles) used to cost a third call without bus access: the CPU
 * counter stayed correct, but the scan got one cycle ahead on every
 * branch not taken — ~410 cycles per frame on the BASIC ROM. */
TEST(test_branch_not_taken_costs_no_phantom_cycle) {
    uint8_t code[] = { 0x18,             /* CLC                : 2 cycles */
                       0xB0, 0x10,       /* BCS +16 (not taken): 2 cycles */
                       0x90, 0x00,       /* BCC +0  (taken)    : 3 cycles */
                       0xEA };           /* NOP                : 2 cycles */
    setup(code, sizeof(code), true);
    for (int i = 0; i < 9; i++) {
        emu_cycle(&g_emu);
        ASSERT_EQ((int)g_emu.cpu.cycles, i + 1);   /* never an idle call */
        ASSERT_EQ(g_emu.raster_cycle, i + 1);
    }
    ASSERT_EQ(g_emu.cpu.PC, 0x0206);
}

/* The whole ROM is the reference: over a BASIC boot frame, the scan and the
 * CPU counter must stay in step (up to the overrun of the last
 * instruction). Without a ROM, a synthetic loop rich in branches
 * not taken plays the same role. */
TEST(test_raster_and_cpu_stay_in_step_over_a_frame) {
    uint8_t code[] = { 0xA2, 0x00,       /* LDX #0 */
                       0xE8,             /* loop: INX */
                       0xF0, 0xFD,       /* BEQ loop (not taken 255 times out of 256) */
                       0xD0, 0xFB };     /* BNE loop (taken) */
    setup(code, sizeof(code), true);
    emu_clock_frame_begin(&g_emu);
    while (g_emu.raster_cycle < CYCLES_PER_FRAME) emu_cycle(&g_emu);
    ASSERT_EQ((int)g_emu.cpu.cycles, g_emu.raster_cycle);
}

int main(void) {
    printf("=== Horloge maître (V2-E2) ===\n\n");
    RUN(test_branch_not_taken_costs_no_phantom_cycle);
    RUN(test_raster_and_cpu_stay_in_step_over_a_frame);
    RUN(test_one_call_is_one_cycle);
    RUN(test_peripherals_get_one_cycle_at_a_time);
    RUN(test_raster_position_advances_by_line);
    RUN(test_frame_is_312_lines_of_64_cycles);
    RUN(test_frame_begin_and_end);
    RUN(test_cpu_write_visible_in_the_same_cell);
    RUN(test_ula_per_cycle_mid_line_split);
    RUN(test_line_render_cannot_split);
    RUN(test_ula_fetch_offset_moves_the_split);
    RUN(test_static_screen_identical_both_paths);
    RUN(test_legacy_core_advances_by_instruction);
    RUN(test_emu_step_returns_instruction_cycles);
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
