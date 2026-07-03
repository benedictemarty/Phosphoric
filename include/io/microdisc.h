/**
 * @file microdisc.h
 * @brief Microdisc disk controller interface for ORIC
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-23
 * @version 1.0.0-beta.7
 *
 * Microdisc I/O addresses:
 *   $0310 R/W - WD1793 Status (R) / Command (W)
 *   $0311 R/W - WD1793 Track
 *   $0312 R/W - WD1793 Sector
 *   $0313 R/W - WD1793 Data
 *   $0314 W   - Control register
 *   $0314 R   - IRQ status (bit 7 = /INTRQ)
 *   $0318 R   - DRQ status (bit 7 = /DRQ)
 */

#ifndef MICRODISC_H
#define MICRODISC_H

#include <stdint.h>
#include <stdbool.h>
#include "storage/disk.h"

/* Forward declaration for emulator (avoids circular include) */
typedef struct emulator_s emulator_t;

/* Microdisc I/O addresses */
#define MICRODISC_BASE     0x0310
#define MICRODISC_FDC_BASE 0x0310  /* $0310-$0313: WD1793 registers */
#define MICRODISC_CTRL     0x0314  /* Control register (write) / IRQ status (read) */
#define MICRODISC_DRQ      0x0318  /* DRQ status (read) */

/* Control register bits ($0314 write) */
#define MICRODISC_CTRL_INTENA   0x01  /* Bit 0: IRQ enable (1=enabled) */
#define MICRODISC_CTRL_ROMDIS   0x02  /* Bit 1: BASIC ROM disable */
#define MICRODISC_CTRL_DENSITY  0x08  /* Bit 3: Density select */
#define MICRODISC_CTRL_SIDE     0x10  /* Bit 4: Side select */
#define MICRODISC_CTRL_DRIVE    0x60  /* Bits 6:5: Drive select */
#define MICRODISC_CTRL_EPROM    0x80  /* Bit 7: 0=overlay ROM active, 1=overlay OFF */

#define MICRODISC_MAX_DRIVES 4

typedef struct microdisc_s {
    fdc_t fdc;                  /* WD1793 FDC */

    /* Microdisc status (active-low convention matching Oricutron) */
    uint8_t status;             /* Control register ($0314 last write) */
    uint8_t intrq;              /* 0x00 = INTRQ active, 0x80 = inactive */
    uint8_t drq;                /* 0x00 = DRQ active, 0x80 = inactive */

    /* Decoded control bits */
    bool diskrom;               /* Overlay ROM visible at $E000-$FFFF */
    bool romdis;                /* BASIC ROM disabled */
    bool intena;                /* FDC IRQ forwarded to CPU */
    uint8_t drive;              /* Selected drive (0-3) */
    uint8_t side;               /* Selected side (0-1) */

    /* Per-drive disk data (4 drives: A, B, C, D) */
    uint8_t* disk_data[MICRODISC_MAX_DRIVES];
    uint32_t disk_size[MICRODISC_MAX_DRIVES];
    uint8_t  disk_tracks[MICRODISC_MAX_DRIVES];
    uint8_t  disk_sectors[MICRODISC_MAX_DRIVES];

    /* Per-drive dirty flag: set when the FDC writes a sector on that drive.
     * Consumed for .dsk write-back on exit (--disk-writeback). */
    bool     disk_dirty[MICRODISC_MAX_DRIVES];

    /* Per-drive bad sector map: damage belongs to the inserted MEDIA.
     * Applied to the FDC on drive select, wiped when a new disk is
     * inserted (microdisc_set_disk), persisted in savestates. */
    fdc_bad_map_t bad_map[MICRODISC_MAX_DRIVES];

    /* Overlay ROM data (microdis.rom, 8KB) */
    uint8_t* diskrom_data;
    uint32_t diskrom_size;

    /* CPU IRQ routing */
    void (*cpu_irq_set)(emulator_t* emu);
    void (*cpu_irq_clr)(emulator_t* emu);
    emulator_t* cpu_userdata;
} microdisc_t;

void microdisc_init(microdisc_t* md);
void microdisc_reset(microdisc_t* md);
uint8_t microdisc_read(microdisc_t* md, uint16_t addr);
void microdisc_write(microdisc_t* md, uint16_t addr, uint8_t value);
void microdisc_set_disk(microdisc_t* md, uint8_t drive, uint8_t* data, uint32_t size,
                        uint8_t tracks, uint8_t sectors_per_track);
/* Mark a sector of the media in DRIVE as unreadable (fault injection).
 * Returns 0 on success, -1 on bad drive or full map. */
int microdisc_add_bad_sector(microdisc_t* md, uint8_t drive,
                             uint8_t side, uint8_t track, uint8_t sector);
/* Re-apply DRIVE's media bad-sector map to the FDC if DRIVE is selected. */
void microdisc_sync_bad_map(microdisc_t* md, uint8_t drive);
bool microdisc_load_rom(microdisc_t* md, const char* filename);
void microdisc_cleanup(microdisc_t* md);

#endif /* MICRODISC_H */
