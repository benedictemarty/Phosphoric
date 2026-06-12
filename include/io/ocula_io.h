/**
 * @file ocula_io.h
 * @brief OCULA identification + memory banking I/O window ($03E0-$03E7)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 * @version 1.20.0-alpha
 *
 * Étape 4 of the OCULA extensions (docs/ocula_extensions.md). Active
 * only under the OCULA ULA profile (--ula ocula). Hardware rationale:
 * the real ULA already decodes the $03xx page (it generates nIO), and
 * in OCULA-is-the-RAM mode it sees the full address bus and drives the
 * data bus — responding on a dedicated $03Ex window is feasible.
 *
 * Registers:
 *   $03E0 R  : 'O' (0x4F)  — identification, mirrors LOCI's 'L' at $0319
 *   $03E1 R  : 'C' (0x43)
 *   $03E2 R  : capability bits (80COL|EXTHIRES|PALETTE|BANKING = 0x0F)
 *   $03E3 R/W: CPU bank for $A000-$BFFF (0-7, masked; 0 = main RAM)
 *   $03E4-$03E7: reserved (read 0x00, writes ignored).
 *               $03E4 is earmarked for a future video-bank register
 *               (page flipping); the ULA always scans bank 0 today.
 */

#ifndef OCULA_IO_H
#define OCULA_IO_H

#include <stdint.h>
#include "memory/memory.h"

#define OCULA_IO_BASE   0x03E0
#define OCULA_IO_END    0x03E7
#define OCULA_IO_ID0    0x03E0
#define OCULA_IO_ID1    0x03E1
#define OCULA_IO_CAPS   0x03E2
#define OCULA_IO_BANK   0x03E3

#define OCULA_CAP_80COL    0x01
#define OCULA_CAP_EXTHIRES 0x02
#define OCULA_CAP_PALETTE  0x04
#define OCULA_CAP_BANKING  0x08
#define OCULA_CAPS_ALL     (OCULA_CAP_80COL | OCULA_CAP_EXTHIRES | \
                            OCULA_CAP_PALETTE | OCULA_CAP_BANKING)

static inline bool ocula_io_addr_in_window(uint16_t address) {
    return address >= OCULA_IO_BASE && address <= OCULA_IO_END;
}

uint8_t ocula_io_read(memory_t* mem, uint16_t address);
void ocula_io_write(memory_t* mem, uint16_t address, uint8_t value);

#endif /* OCULA_IO_H */
