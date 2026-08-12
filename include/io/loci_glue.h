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

/* --- Epic 9 / US3 : ROM / tape / resume callbacks & host helpers --- */
/* Fill OUT with the LOCI resume-snapshot path (.ost next to the SD image). */
void loci_resume_snapshot_path(emulator_t* emu, char* out, size_t outsz);
/* Locate a ROM file under the LOCI flash root; true + path in OUT if found. */
bool loci_find_rom_file(emulator_t* emu, const char* name, char* out, size_t outsz);
/* Locate the LOCI menu ROM. */
bool loci_find_menu_rom(emulator_t* emu, char* out, size_t outsz);
/* Patch the LOCI firmware ROM-info block in memory. */
void loci_patch_rom_info(emulator_t* emu);
/* LOCI callback: mount a host TAP into the tape buffer for CLOAD. */
bool loci_tape_mount_cb(void* ctx, const char* host_tape_path);
/* LOCI callback: swap a ROM image into memory at base_addr and reset. */
bool loci_rom_swap_cb(void* ctx, const char* rom_path, uint16_t base_addr);
/* LOCI callback: restore a resume session snapshot. */
bool loci_resume_session_cb(void* ctx);

#endif /* LOCI_GLUE_H */
