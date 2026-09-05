/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file jasmin.h
 * @brief Jasmin disk interface for ORIC (WD177x FDC) — 2nd Oric disk standard
 * @author bmarty <bmarty@mailo.com>
 *
 * Jasmin (Tangerine's own disk interface, competitor to the Microdisc). The
 * register map below is confirmed against Oricutron (disk.c jasmin_read/
 * jasmin_write) — nothing invented.
 *
 * Jasmin I/O addresses (page $03Fx):
 *   $03F4-$03F7 R/W - WD177x FDC registers (addr & 3: status/cmd, track,
 *                     sector, data). The Jasmin FDC is a WD1770/1772: unlike
 *                     the Microdisc's WD1793 it has NO side-select pin — the
 *                     side is driven externally by $03F8.
 *   $03F8 W        - Side select (bit 0)
 *   $03F9 W        - Disk controller reset
 *   $03FA W        - Overlay RAM enable (olay)
 *   $03FB R/W      - ROMDIS: disable the BASIC ROM (bit 0). Read returns romdis.
 *   $03FC W        - Select drive 0
 *   $03FD W        - Select drive 1
 *   $03FE W        - Select drive 2
 *   $03FF W        - Select drive 3
 *
 * Jasmin boot ROM: 2 KB mapped at $F800-$FFFF. On RESET the Jasmin ROM is
 * paged in and auto-boots from disk; software controls paging via ROMDIS/olay.
 */

#ifndef JASMIN_H
#define JASMIN_H

#include <stdint.h>
#include <stdbool.h>
#include "storage/disk.h"

/* Forward declaration for emulator (avoids circular include) */
typedef struct emulator_s emulator_t;

/* Jasmin I/O addresses (confirmed vs Oricutron) */
#define JASMIN_BASE        0x03F4
#define JASMIN_FDC_BASE    0x03F4  /* $03F4-$03F7: WD177x registers (addr & 3) */
#define JASMIN_SIDE        0x03F8  /* Side select (bit 0)                     */
#define JASMIN_RESET       0x03F9  /* Disk controller reset                   */
#define JASMIN_OLAY        0x03FA  /* Overlay RAM enable                      */
#define JASMIN_ROMDIS      0x03FB  /* BASIC ROM disable (bit 0)               */
#define JASMIN_DRIVE0      0x03FC  /* Select drive 0                          */
#define JASMIN_DRIVE1      0x03FD  /* Select drive 1                          */
#define JASMIN_DRIVE2      0x03FE  /* Select drive 2                          */
#define JASMIN_DRIVE3      0x03FF  /* Select drive 3                          */
#define JASMIN_END         0x03FF

/* Jasmin boot ROM: 2 KB at $F800-$FFFF */
#define JASMIN_ROM_BASE    0xF800
#define JASMIN_ROM_SIZE    0x0800  /* 2 KB */

#define JASMIN_MAX_DRIVES  4

typedef struct jasmin_s {
    fdc_t fdc;                  /* WD177x FDC (reuses the WD1793 core) */

    /* Latched control state (Oricutron parity) */
    bool    romdis;            /* $03FB: BASIC ROM disabled                */
    bool    olay;              /* $03FA: overlay RAM enabled               */
    uint8_t drive;             /* Selected drive (0-3) via $03FC-$03FF     */
    uint8_t side;              /* $03F8: selected side (0-1)               */

    /* INTRQ/DRQ latches (active-low convention like the Microdisc).
     * NOTE: on the Jasmin it is DRQ — not INTRQ — that drives the CPU IRQ
     * line (IRQF_DISK); INTRQ is not wired to the CPU (Oricutron
     * jasmin_setintrq is a no-op). There is no INTENA gate. */
    uint8_t intrq;             /* 0x00 = INTRQ active, 0x80 = inactive     */
    uint8_t drq;               /* 0x00 = DRQ active, 0x80 = inactive       */

    /* Jasmin boot ROM ($F800-$FFFF), loaded via --jasmin-rom */
    uint8_t rom[JASMIN_ROM_SIZE];
    bool    rom_valid;         /* true once a 2 KB Jasmin ROM is loaded    */
    bool    autoboot_done;     /* one-shot: ROM-PC-trap auto-boot fired     */

    /* Per-drive disk data (A, B, C, D) — same model as the Microdisc */
    uint8_t* disk_data[JASMIN_MAX_DRIVES];
    uint32_t disk_size[JASMIN_MAX_DRIVES];
    uint8_t  disk_tracks[JASMIN_MAX_DRIVES];
    uint8_t  disk_sectors[JASMIN_MAX_DRIVES];
    bool     disk_dirty[JASMIN_MAX_DRIVES];

    /* Per-drive bad-sector map: damage belongs to the inserted media. */
    fdc_bad_map_t bad_map[JASMIN_MAX_DRIVES];

    /* CPU IRQ line (IRQF_DISK). Driven by DRQ on the Jasmin. Wired in main.c
     * to keep this module decoupled from the CPU (same as the Microdisc). */
    void (*cpu_irq_set)(emulator_t* emu);
    void (*cpu_irq_clr)(emulator_t* emu);
    emulator_t* cpu_userdata;
} jasmin_t;

/* Lifecycle */
void jasmin_init(jasmin_t* j);
void jasmin_reset(jasmin_t* j);

/* CPU I/O (routed by io_bus for $03F4-$03FF) */
uint8_t jasmin_read(jasmin_t* j, uint16_t addr);
void    jasmin_write(jasmin_t* j, uint16_t addr, uint8_t value);

/* ROM + media */
bool jasmin_load_rom(jasmin_t* j, const uint8_t* data, uint32_t size);
void jasmin_set_disk(jasmin_t* j, uint8_t drive, uint8_t* data, uint32_t size,
                     uint8_t tracks, uint8_t sectors_per_track);

/* Mark a sector unreadable (RNF/CRC) on @p drive — mirrors the Microdisc; the
 * damage belongs to the media and is swapped in on drive select. Returns 0 on
 * success, -1 on out-of-range drive or a full map. */
int jasmin_add_bad_sector(jasmin_t* j, uint8_t drive,
                          uint8_t side, uint8_t track, uint8_t sector);

#endif /* JASMIN_H */
