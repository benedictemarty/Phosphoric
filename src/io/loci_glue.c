/**
 * @file loci_glue.c
 * @brief LOCI ↔ emulator adapter callbacks — moved verbatim from main.c (Epic 9).
 * @author bmarty <bmarty@mailo.com>
 */
#include "io/loci_glue.h"
#include "cpu/cpu6502.h"   /* cpu_irq_set/clear, IRQF_DISK */

void loci_dsk_cpu_irq_set(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    cpu_irq_set(&emu->cpu, IRQF_DISK);
}

void loci_dsk_cpu_irq_clr(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    cpu_irq_clear(&emu->cpu, IRQF_DISK);
}

void loci_dsk_sync_overlay(void* ctx, bool basic_disabled, bool overlay_active) {
    emulator_t* emu = (emulator_t*)ctx;
    emu->memory.basic_rom_disabled = basic_disabled;
    emu->memory.overlay_active     = overlay_active;
}

void loci_rom_poke_hook(void* ctx, uint16_t addr, uint8_t val) {
    emulator_t* emu = (emulator_t*)ctx;
    if (emu && addr >= 0xC000)
        emu->memory.rom[addr - 0xC000] = val;
}
