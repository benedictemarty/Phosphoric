/**
 * @file loci_glue.h
 * @brief LOCI ↔ emulator adapter callbacks (extracted from main.c, Epic 9).
 * @author bmarty <bmarty@mailo.com>
 *
 * The LOCI core (src/io/loci_*.c) is kept free of emulator.h. These callbacks
 * are the single place that knows both the LOCI and emulator_t — the same
 * adapter role as io_bus.c. Each takes the emulator as `void* ctx`, matching the
 * loci_set_*_callback() registration seam used in main().
 */
#ifndef LOCI_GLUE_H
#define LOCI_GLUE_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator.h"

/* Microdisc-compatible disk IRQ line served by the LOCI DSK window. */
void loci_dsk_cpu_irq_set(void* ctx);
void loci_dsk_cpu_irq_clr(void* ctx);
/* Mirror the LOCI overlay/BASIC-ROM banking state into the memory map. */
void loci_dsk_sync_overlay(void* ctx, bool basic_disabled, bool overlay_active);
/* LOCI ROM poke hook: write a byte into the overlay ROM image ($C000+). */
void loci_rom_poke_hook(void* ctx, uint16_t addr, uint8_t val);

#endif /* LOCI_GLUE_H */
