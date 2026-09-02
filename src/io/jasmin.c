/**
 * @file jasmin.c
 * @brief Jasmin disk interface for ORIC (WD177x FDC)
 * @author bmarty <bmarty@mailo.com>
 *
 * Register map, memory mapping and IRQ wiring are confirmed against Oricutron
 * (disk.c jasmin_read/jasmin_write/jasmin_setdrq, machine.c) — nothing invented.
 *
 * Key difference from the Microdisc: on the Jasmin it is DRQ (not INTRQ) that
 * drives the CPU IRQ line (IRQF_DISK); INTRQ is not wired to the CPU and there
 * is no INTENA gate. The FDC core is the shared WD1793 emulation; the Jasmin's
 * WD1770/1772 has no SIDE pin, so the side is driven externally via $03F8.
 */

#include "io/jasmin.h"
#include <string.h>

/* ── FDC → Jasmin callbacks ──────────────────────────────────────────
 * DRQ is the IRQ source on the Jasmin (Oricutron jasmin_setdrq → cpu.irq |=
 * IRQF_DISK). INTRQ only latches the status bit (jasmin_setintrq is a no-op). */

static void jasmin_fdc_set_drq(void* userdata) {
    jasmin_t* j = (jasmin_t*)userdata;
    j->drq = 0x00;                       /* DRQ active */
    if (j->cpu_irq_set) j->cpu_irq_set(j->cpu_userdata);
}

static void jasmin_fdc_clr_drq(void* userdata) {
    jasmin_t* j = (jasmin_t*)userdata;
    j->drq = 0x80;                       /* DRQ inactive */
    if (j->cpu_irq_clr) j->cpu_irq_clr(j->cpu_userdata);
}

static void jasmin_fdc_set_intrq(void* userdata) {
    jasmin_t* j = (jasmin_t*)userdata;
    j->intrq = 0x00;                     /* INTRQ active (status only) */
}

static void jasmin_fdc_clr_intrq(void* userdata) {
    jasmin_t* j = (jasmin_t*)userdata;
    j->intrq = 0x80;                     /* INTRQ inactive */
}

static void jasmin_select_drive(jasmin_t* j, uint8_t drive) {
    if (drive >= JASMIN_MAX_DRIVES) return;
    j->drive = drive;
    fdc_set_disk(&j->fdc, j->disk_data[drive], j->disk_size[drive]);
    j->fdc.tracks = j->disk_tracks[drive];
    j->fdc.sectors_per_track = j->disk_sectors[drive];
    fdc_set_bad_map(&j->fdc, &j->bad_map[drive]);
}

void jasmin_init(jasmin_t* j) {
    /* Preserve any ROM/media already installed by the CLI before init. */
    fdc_init(&j->fdc);
    j->fdc.timing_mode = FDC_TIMING_REAL;   /* real 3"/5.25" mechanism */

    j->fdc.set_drq = jasmin_fdc_set_drq;
    j->fdc.clr_drq = jasmin_fdc_clr_drq;
    j->fdc.drq_userdata = j;
    j->fdc.set_intrq = jasmin_fdc_set_intrq;
    j->fdc.clr_intrq = jasmin_fdc_clr_intrq;
    j->fdc.intrq_userdata = j;

    /* Boot state matches Oricutron jasmin_init: BASIC ROM visible, Jasmin ROM
     * paged in later by the auto-boot trap (ROMDIS write at $EB78/$E905). */
    j->romdis = false;
    j->olay   = false;
    j->drive  = 0;
    j->side   = 0;
    j->intrq  = 0x80;
    j->drq    = 0x80;
}

void jasmin_reset(jasmin_t* j) {
    fdc_reset(&j->fdc);
    j->romdis = false;
    j->olay   = false;
    j->drive  = 0;
    j->side   = 0;
    j->intrq  = 0x80;
    j->drq    = 0x80;
}

uint8_t jasmin_read(jasmin_t* j, uint16_t addr) {
    if (addr >= JASMIN_FDC_BASE && addr <= (JASMIN_FDC_BASE + 3)) {
        return fdc_read(&j->fdc, (uint8_t)(addr & 3));
    }
    if (addr == JASMIN_ROMDIS) {
        return j->romdis ? 1 : 0;        /* Oricutron: read returns romdis */
    }
    return 0xFF;
}

void jasmin_write(jasmin_t* j, uint16_t addr, uint8_t value) {
    if (addr >= JASMIN_FDC_BASE && addr <= (JASMIN_FDC_BASE + 3)) {
        fdc_write(&j->fdc, (uint8_t)(addr & 3), value);
        if (j->fdc.disk_modified) {
            j->disk_dirty[j->drive & 3] = true;
            j->fdc.disk_modified = false;
        }
        return;
    }

    switch (addr) {
    case JASMIN_SIDE:                    /* $03F8: side select (bit 0) */
        j->side = value & 1;
        j->fdc.side = j->side;
        break;

    case JASMIN_RESET:                   /* $03F9: controller reset */
        fdc_reset(&j->fdc);
        j->intrq = 0x80;
        j->drq   = 0x80;
        if (j->cpu_irq_clr) j->cpu_irq_clr(j->cpu_userdata);
        break;

    case JASMIN_OLAY:                    /* $03FA: overlay RAM enable */
        j->olay = (value != 0);
        break;

    case JASMIN_ROMDIS:                  /* $03FB: disable BASIC ROM (bit 0) */
        j->romdis = (value & 1) != 0;
        break;

    case JASMIN_DRIVE0:                  /* $03FC-$03FF: select drive 0-3 */
    case JASMIN_DRIVE1:
    case JASMIN_DRIVE2:
    case JASMIN_DRIVE3: {
        uint8_t new_drive = (uint8_t)(addr & 3);
        if (new_drive != j->drive) jasmin_select_drive(j, new_drive);
        break;
    }
    default:
        break;
    }
}

bool jasmin_load_rom(jasmin_t* j, const uint8_t* data, uint32_t size) {
    if (!data || size != JASMIN_ROM_SIZE) return false;   /* Jasmin ROM = 2 KB */
    memcpy(j->rom, data, JASMIN_ROM_SIZE);
    j->rom_valid = true;
    return true;
}

int jasmin_add_bad_sector(jasmin_t* j, uint8_t drive,
                          uint8_t side, uint8_t track, uint8_t sector) {
    if (drive >= JASMIN_MAX_DRIVES) return -1;
    if (fdc_bad_map_add(&j->bad_map[drive], side, track, sector) != 0) return -1;
    /* Re-point the live FDC map if this is the selected drive. */
    if (drive == j->drive)
        fdc_set_bad_map(&j->fdc, &j->bad_map[drive]);
    return 0;
}

void jasmin_set_disk(jasmin_t* j, uint8_t drive, uint8_t* data, uint32_t size,
                     uint8_t tracks, uint8_t sectors_per_track) {
    if (drive >= JASMIN_MAX_DRIVES) return;
    bool media_changed = (j->disk_data[drive] != data);
    j->disk_data[drive] = data;
    j->disk_size[drive] = size;
    j->disk_tracks[drive] = tracks;
    j->disk_sectors[drive] = sectors_per_track;
    if (media_changed)
        memset(&j->bad_map[drive], 0, sizeof(j->bad_map[drive]));
    if (drive == j->drive) {
        fdc_set_disk(&j->fdc, data, size);
        j->fdc.tracks = tracks;
        j->fdc.sectors_per_track = sectors_per_track;
        fdc_set_bad_map(&j->fdc, &j->bad_map[drive]);
    }
}
