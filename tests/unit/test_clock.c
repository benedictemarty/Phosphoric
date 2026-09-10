/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_clock.c
 * @brief Horloge maître : un cycle de machine, ordre intra-cycle (V2-E2)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * Vérifie le contrat de `emu_cycle()` (src/emu_clock.c) :
 *   - un appel = un cycle CPU exactement, et la position du balayage avance avec ;
 *   - l'ordre intra-cycle **φ1 ULA → φ2 CPU** : une écriture du CPU pendant le
 *     cycle *c* n'est PAS visible par la scanline émise à ce même cycle ; elle ne
 *     l'est qu'à partir du cycle suivant (c'est la convention de visibilité du
 *     matériel, où l'ULA accède à la RAM en φ1) ;
 *   - les périphériques φ2 reçoivent bien **un** cycle à la fois (pas de paquet) ;
 *   - la trame PAL compte 312 lignes de 64 cycles et 224 lignes visibles.
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

/* Un émulateur minimal : mémoire, CPU, vidéo, VIA, ULA-NG. Pas de périphérique
 * de stockage ni de son — l'horloge n'en a pas besoin. */
static emulator_t g_emu;

/* Rappel d'horloge identique en esprit à celui de main.c : il sert ici à
 * compter les cycles livrés aux périphériques φ2. */
static int g_tick_calls, g_tick_cycles, g_tick_max;
static void clock_tick(void* ctx, int cycles) {
    emulator_t* emu = (emulator_t*)ctx;
    via_update(&emu->via, cycles);
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

/* Compte les pixels non noirs de la ligne `y` du framebuffer. */
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
        ASSERT_EQ((int)g_emu.cpu.cycles, i);       /* un cycle CPU par appel */
        ASSERT_EQ(g_emu.raster_cycle, i);          /* le balayage suit */
    }
    ASSERT_EQ(g_emu.frame_cycles, 8);
}

TEST(test_peripherals_get_one_cycle_at_a_time) {
    uint8_t code[] = { 0xA9, 0x05, 0x85, 0x10, 0xE6, 0x10,
                       0xA2, 0x03, 0xFE, 0x00, 0x04, 0x48, 0x68 };
    setup(code, sizeof(code), true);
    for (int i = 0; i < 40; i++) emu_cycle(&g_emu);
    ASSERT_EQ(g_tick_cycles, 40);
    ASSERT_EQ(g_tick_calls, 40);     /* un appel par cycle… */
    ASSERT_EQ(g_tick_max, 1);        /* …et jamais de paquet */
}

TEST(test_raster_position_advances_by_line) {
    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    for (int i = 0; i < PAL_CYCLES_PER_LINE * 3; i++) emu_cycle(&g_emu);
    int line = -1, dot = -1;
    emu_raster_pos(&g_emu, &line, &dot);
    ASSERT_EQ(line, 3);
    ASSERT_EQ(dot, 0);
    ASSERT_EQ(g_emu.raster_rendered, 3);   /* lignes 0,1,2 émises */
}

TEST(test_frame_is_312_lines_of_64_cycles) {
    ASSERT_EQ(PAL_CYCLES_PER_LINE, 64);
    ASSERT_EQ(CYCLES_PER_FRAME, 312 * 64);
    ASSERT_EQ(ULA_NG_FRAME_LINES, 312);

    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    for (int i = 0; i < CYCLES_PER_FRAME; i++) emu_cycle(&g_emu);
    /* Zone visible : 224 lignes rendues, le reste est du blanking. */
    ASSERT_EQ(g_emu.raster_rendered, 224);
    ASSERT_EQ(g_emu.raster_ng_line, 312);
}

TEST(test_frame_begin_and_end) {
    uint8_t code[] = { 0xEA };
    setup(code, sizeof(code), true);
    for (int i = 0; i < 100; i++) emu_cycle(&g_emu);
    ASSERT_EQ(g_emu.raster_rendered, 1);
    emu_clock_frame_end(&g_emu);           /* termine la trame interrompue */
    ASSERT_EQ(g_emu.raster_rendered, 224);
    emu_clock_frame_begin(&g_emu);
    ASSERT_EQ(g_emu.raster_cycle, 0);
    ASSERT_EQ(g_emu.raster_rendered, 0);
    ASSERT_EQ(g_emu.raster_ng_line, 0);
}

/* ── Le test d'ordre intra-cycle (US2.3) ──
 * L'écriture du CPU tombe exactement au cycle 64, celui où la scanline 0 est
 * émise. Si l'ULA passe bien AVANT le CPU (φ1 puis φ2), la ligne 0 montre
 * l'ancien contenu ; la ligne 1, émise au cycle 128, montre le nouveau. */
TEST(test_ula_reads_before_cpu_writes) {
    /* 30 NOP (60 cycles) puis STA $BB80 : fetches aux cycles 61-63, écriture au
     * cycle 64 — exactement le cycle où la ligne 0 est émise. */
    uint8_t code[128];
    size_t n = 0;
    for (int i = 0; i < 30; i++) code[n++] = 0xEA;
    code[n++] = 0x8D; code[n++] = 0x80; code[n++] = 0xBB;   /* STA $BB80 */
    setup(code, n, true);

    /* Écran : 'A' partout ; le CPU va écrire un espace en $BB80. */
    memset(&g_emu.memory.ram[0xBB80], 'A', 40 * 28);
    memset(&g_emu.memory.ram[0xB400], 0x3F, 128 * 8);   /* charset : tout encre… */
    memset(&g_emu.memory.ram[0xB400 + ' ' * 8], 0x00, 8);  /* …sauf l'espace, vide */
    g_emu.cpu.A = ' ';

    for (int i = 0; i < 64; i++) emu_cycle(&g_emu);

    /* L'écriture a bien eu lieu pendant ce 64e cycle… */
    ASSERT_EQ((int)g_emu.cpu.cycles, 64);
    ASSERT_EQ(g_emu.memory.ram[0xBB80], ' ');
    /* …mais la ligne 0 avait déjà été émise : elle porte encore de l'encre. */
    ASSERT_EQ(g_emu.raster_rendered, 1);
    int ink0 = line_ink(&g_emu.video, 0);
    ASSERT_TRUE(ink0 > 0);

    /* La ligne 1, émise au cycle 128, voit le changement : la première cellule
     * devient vide, donc elle porte STRICTEMENT moins d'encre que la ligne 0. */
    for (int i = 0; i < PAL_CYCLES_PER_LINE; i++) emu_cycle(&g_emu);
    ASSERT_EQ(g_emu.raster_rendered, 2);
    int ink1 = line_ink(&g_emu.video, 1);
    ASSERT_TRUE(ink1 < ink0);
}

/* Le cœur historique ne sait pas s'arrêter entre deux cycles : emu_cycle() y
 * exécute une instruction entière, et le balayage rattrape d'autant. */
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

int main(void) {
    printf("=== Horloge maître (V2-E2) ===\n\n");
    RUN(test_one_call_is_one_cycle);
    RUN(test_peripherals_get_one_cycle_at_a_time);
    RUN(test_raster_position_advances_by_line);
    RUN(test_frame_is_312_lines_of_64_cycles);
    RUN(test_frame_begin_and_end);
    RUN(test_ula_reads_before_cpu_writes);
    RUN(test_legacy_core_advances_by_instruction);
    RUN(test_emu_step_returns_instruction_cycles);
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
