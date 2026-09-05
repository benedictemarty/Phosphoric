/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file banking.c
 * @brief Memory banking implementation (ROM/RAM overlay)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.3.0-alpha
 *
 * ORIC-1 banking: controlled via bit in the VIA Port B.
 * When overlay active, writes go to RAM under ROM area ($C000-$FFFF).
 */

#include "memory/memory.h"

void memory_set_rom_enabled(memory_t* mem, bool enabled) {
    mem->rom_enabled = enabled;
}

bool memory_get_rom_enabled(const memory_t* mem) {
    return mem->rom_enabled;
}

void memory_set_charset_bank(memory_t* mem, memory_bank_t bank) {
    mem->charset_bank = bank;
}
