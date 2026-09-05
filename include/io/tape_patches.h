/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file tape_patches.h
 * @brief ROM CLOAD/CSAVE PC-matching patches (extracted from main.c).
 * @author bmarty <bmarty@mailo.com>
 */
#ifndef TAPE_PATCHES_H
#define TAPE_PATCHES_H

#include "emulator.h"

/* Intercept the ROM cassette routines by matching the CPU PC after each
 * instruction (CLOAD read patches + CSAVE write capture). Called once per
 * instruction from the emulation loop. Behaviour is verbatim from the former
 * static main.c function; covered by test-tape-roundtrip and the CLOAD tests. */
void tape_patches(emulator_t* emu);

#endif /* TAPE_PATCHES_H */
