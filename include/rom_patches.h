/**
 * @file rom_patches.h
 * @brief ROM-version patch tables + model detection/selection.
 * @author bmarty <bmarty@mailo.com>
 *
 * Owns the BASIC 1.0/1.1 tape-patch address tables (formerly static in main.c).
 * Exposed as a LIB translation unit so both main.c and the LOCI adapter
 * (loci_glue.c, which re-selects patches after a ROM swap) can link them
 * (Epic 9/US3).
 */
#ifndef ROM_PATCHES_H
#define ROM_PATCHES_H

#include "emulator.h"
#include "memory/memory.h"

/* Auto-detect the machine model from the loaded ROM (JMP target at $C000). */
oric_model_t detect_rom_version(const memory_t* mem);
/* Select the ROM-patch table for a model. */
const rom_patches_t* get_rom_patches(oric_model_t model);

#endif /* ROM_PATCHES_H */
