/**
 * @file ocula_io.c
 * @brief OCULA identification + memory banking I/O window ($03E0-$03E7)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 * @version 1.20.0-alpha
 */

#include "io/ocula_io.h"

uint8_t ocula_io_read(memory_t* mem, uint16_t address) {
    switch (address) {
        case OCULA_IO_ID0:  return 'O';
        case OCULA_IO_ID1:  return 'C';
        case OCULA_IO_CAPS: return OCULA_CAPS_ALL;
        case OCULA_IO_BANK: return memory_ocula_get_bank(mem);
        default:            return 0x00;  /* reserved */
    }
}

void ocula_io_write(memory_t* mem, uint16_t address, uint8_t value) {
    if (address == OCULA_IO_BANK) {
        memory_ocula_set_bank(mem, (uint8_t)(value & 0x07));
    }
    /* ID/caps/reserved: writes ignored */
}
