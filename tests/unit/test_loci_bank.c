/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_loci_bank.c
 * @brief Banking LOCI $A7 (SET_BANK) de bout en bout : banque xram -> $A7 ->
 *        loci_bank_swap_cb -> memory_read($C000-$FFFF), avec la vraie colle.
 * @author bmarty <bmarty@mailo.com>
 *
 * Ce que ces tests ne prouvent PAS : le comportement du vrai matériel (timing MAP
 * pendant la commutation à chaud, banque inscriptible). Voir docs/loci.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "emulator.h"
#include "io/loci.h"
#include "io/loci_glue.h"
#include "memory/memory.h"
#include "cpu/cpu6502.h"
#include "utils/logging.h"

static int g_failed = 0;

#define RUN(name) do { \
    printf("  %-52s ", #name); \
    if (name()) printf("PASS\n"); else { printf("FAIL\n"); g_failed++; } \
} while (0)

#define CHECK(expr) do { \
    if (!(expr)) { printf("[%s:%d %s] ", __FILE__, __LINE__, #expr); return false; } \
} while (0)

static void set_bank(loci_t* l, uint8_t a) {
    l->regs[LOCI_REG_API_A] = a;
    loci_write(l, 0x03AF, LOCI_OP_SET_BANK);
}

static emulator_t* new_emu(void) {
    emulator_t* emu = calloc(1, sizeof(*emu));
    if (!emu) return NULL;
    if (!memory_init(&emu->memory)) { free(emu); return NULL; }
    cpu_init(&emu->cpu, &emu->memory);
    if (!loci_init(&emu->loci)) { free(emu); return NULL; }
    emu->loci.enabled = true;
    loci_set_bank_swap_callback(&emu->loci, loci_bank_swap_cb, emu);
    return emu;
}

static void free_emu(emulator_t* emu) {
    loci_cleanup(&emu->loci);
    free(emu);
}

static bool test_bank_served_at_c000_and_restored(void) {
    emulator_t* emu = new_emu();
    CHECK(emu != NULL);
    uint8_t before_c000 = memory_read(&emu->memory, 0xC000);
    uint8_t before_fff0 = memory_read(&emu->memory, 0xFFF0);

    uint32_t base = 2u * 0x4000u;
    emu->loci.xram[base + 0x0000] = 0xA5;
    emu->loci.xram[base + 0x0001] = 0x42;
    emu->loci.xram[base + 0x3F00] = 0x77;   /* page $FF : plus de zone réservée */
    emu->loci.xram[base + 0x3FFA] = 0x99;   /* vecteur NMI */

    set_bank(&emu->loci, 0x80 | 2);
    CHECK(emu->loci.bank_enabled && emu->loci.bank_sel == 2);
    CHECK(memory_read(&emu->memory, 0xC000) == 0xA5);
    CHECK(memory_read(&emu->memory, 0xC001) == 0x42);
    CHECK(memory_read(&emu->memory, 0xFF00) == 0x77);
    CHECK(memory_read(&emu->memory, 0xFFFA) == 0x99);

    set_bank(&emu->loci, 0x00);
    CHECK(memory_read(&emu->memory, 0xC000) == before_c000);
    CHECK(memory_read(&emu->memory, 0xFFF0) == before_fff0);
    free_emu(emu);
    return true;
}

static bool test_hot_switch_keeps_cpu_running(void) {
    emulator_t* emu = new_emu();
    CHECK(emu != NULL);
    emu->loci.xram[1u * 0x4000u] = 0x11;
    emu->loci.xram[3u * 0x4000u] = 0x33;

    set_bank(&emu->loci, 0x80 | 1);
    CHECK(memory_read(&emu->memory, 0xC000) == 0x11);

    /* Le firmware ne remet pas le 6502 à zéro : PC/SP doivent survivre. */
    emu->cpu.PC = 0x0600;
    emu->cpu.SP = 0xE0;
    set_bank(&emu->loci, 0x80 | 3);
    CHECK(emu->loci.bank_sel == 3);
    CHECK(memory_read(&emu->memory, 0xC000) == 0x33);
    CHECK(emu->cpu.PC == 0x0600 && emu->cpu.SP == 0xE0);
    free_emu(emu);
    return true;
}

static bool test_disable_without_enable_is_noop(void) {
    emulator_t* emu = new_emu();
    CHECK(emu != NULL);
    uint8_t before = memory_read(&emu->memory, 0xC000);
    set_bank(&emu->loci, 0x00);
    CHECK(memory_read(&emu->memory, 0xC000) == before);
    free_emu(emu);
    return true;
}

static bool test_bank_restores_microdisc_flags(void) {
    emulator_t* emu = new_emu();
    CHECK(emu != NULL);
    emu->memory.overlay_active     = true;    /* état Microdisc avant banking */
    emu->memory.basic_rom_disabled = true;
    set_bank(&emu->loci, 0x80 | 1);
    CHECK(!emu->memory.overlay_active && !emu->memory.basic_rom_disabled);
    set_bank(&emu->loci, 0x00);
    CHECK(emu->memory.overlay_active && emu->memory.basic_rom_disabled);
    free_emu(emu);
    return true;
}

int main(void) {
    log_init(LOG_LEVEL_ERROR);
    printf("\n=== LOCI banking $A7 (SET_BANK -> $C000-$FFFF) ===\n\n");
    RUN(test_bank_served_at_c000_and_restored);
    RUN(test_hot_switch_keeps_cpu_running);
    RUN(test_disable_without_enable_is_noop);
    RUN(test_bank_restores_microdisc_flags);
    printf("\n%s\n", g_failed ? "ECHEC" : "Tous les tests de banking passent");
    return g_failed ? 1 : 0;
}
