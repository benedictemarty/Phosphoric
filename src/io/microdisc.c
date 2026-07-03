/**
 * @file microdisc.c
 * @brief Microdisc disk controller interface for ORIC
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-23
 * @version 1.0.0-beta.7
 *
 * Emulates the Microdisc interface connecting WD1793 FDC to ORIC bus.
 * Uses active-low DRQ/INTRQ convention matching Oricutron:
 * - intrq = 0x00 when INTRQ is asserted (active)
 * - intrq = 0x80 when INTRQ is cleared (inactive)
 * - drq   = 0x00 when DRQ is asserted (active)
 * - drq   = 0x80 when DRQ is cleared (inactive)
 */

#include "io/microdisc.h"
#include "emulator.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* DRQ/INTRQ callbacks from FDC to Microdisc */
static void microdisc_fdc_set_drq(void* userdata) {
    microdisc_t* md = (microdisc_t*)userdata;
    md->drq = 0x00; /* DRQ active */
}

static void microdisc_fdc_clr_drq(void* userdata) {
    microdisc_t* md = (microdisc_t*)userdata;
    md->drq = 0x80; /* DRQ inactive */
}

static void microdisc_fdc_set_intrq(void* userdata) {
    microdisc_t* md = (microdisc_t*)userdata;
    md->intrq = 0x00; /* INTRQ active */
    /* If INTENA is set, forward to CPU */
    if ((md->status & MICRODISC_CTRL_INTENA) && md->cpu_irq_set) {
        md->cpu_irq_set(md->cpu_userdata);
    }
}

static void microdisc_fdc_clr_intrq(void* userdata) {
    microdisc_t* md = (microdisc_t*)userdata;
    md->intrq = 0x80; /* INTRQ inactive */
    if (md->cpu_irq_clr) {
        md->cpu_irq_clr(md->cpu_userdata);
    }
}

static void microdisc_select_drive(microdisc_t* md, uint8_t drive) {
    if (drive >= MICRODISC_MAX_DRIVES) return;
    md->drive = drive;
    fdc_set_disk(&md->fdc, md->disk_data[drive], md->disk_size[drive]);
    md->fdc.tracks = md->disk_tracks[drive];
    md->fdc.sectors_per_track = md->disk_sectors[drive];
    /* Damage follows the media: load this drive's media map */
    fdc_set_bad_map(&md->fdc, &md->bad_map[drive]);
}

void microdisc_init(microdisc_t* md) {
    memset(md, 0, sizeof(microdisc_t));
    fdc_init(&md->fdc);

    /* Wire FDC callbacks to Microdisc */
    md->fdc.set_drq = microdisc_fdc_set_drq;
    md->fdc.clr_drq = microdisc_fdc_clr_drq;
    md->fdc.drq_userdata = md;
    md->fdc.set_intrq = microdisc_fdc_set_intrq;
    md->fdc.clr_intrq = microdisc_fdc_clr_intrq;
    md->fdc.intrq_userdata = md;

    md->diskrom = true;   /* Boot: overlay ROM visible */
    md->romdis = true;    /* Boot: BASIC ROM disabled */
    md->intena = false;
    md->intrq = 0x80;     /* No INTRQ */
    md->drq = 0x80;       /* No DRQ */
    md->drive = 0;
    md->side = 0;
}

void microdisc_reset(microdisc_t* md) {
    fdc_reset(&md->fdc);
    md->status = 0;
    md->intrq = 0x80;
    md->drq = 0x80;
    md->diskrom = true;
    md->romdis = true;
    md->intena = false;
    md->drive = 0;
    md->side = 0;
}

uint8_t microdisc_read(microdisc_t* md, uint16_t addr) {
    if (addr >= 0x0310 && addr <= 0x0313) {
        return fdc_read(&md->fdc, (uint8_t)(addr & 3));
    }

    if (addr == 0x0314) {
        /* IRQ status: bit 7 = /INTRQ (active low), bits 6:0 = 1 */
        return md->intrq | 0x7F;
    }

    if (addr == 0x0318) {
        /* DRQ status: bit 7 = /DRQ (active low), bits 6:0 = 1 */
        return md->drq | 0x7F;
    }

    return 0xFF;
}

void microdisc_write(microdisc_t* md, uint16_t addr, uint8_t value) {
    if (addr >= 0x0310 && addr <= 0x0313) {
        fdc_write(&md->fdc, (uint8_t)(addr & 3), value);
        /* If the FDC just mutated the in-memory image, mark this drive dirty. */
        if (md->fdc.disk_modified) {
            md->disk_dirty[md->drive & 3] = true;
            md->fdc.disk_modified = false;
        }
        return;
    }

    if (addr == 0x0314) {
        md->status = value;

        /* If INTENA set and /INTRQ active (0), assert CPU IRQ */
        if ((value & MICRODISC_CTRL_INTENA) && (md->intrq == 0x00)) {
            if (md->cpu_irq_set) md->cpu_irq_set(md->cpu_userdata);
        } else {
            if (md->cpu_irq_clr) md->cpu_irq_clr(md->cpu_userdata);
        }

        /* Decode control bits */
        md->intena = (value & MICRODISC_CTRL_INTENA) != 0;
        md->romdis = (value & MICRODISC_CTRL_ROMDIS) == 0;  /* Bit 1: 0=ROM disabled */
        md->side = (value & MICRODISC_CTRL_SIDE) ? 1 : 0;
        md->diskrom = (value & MICRODISC_CTRL_EPROM) == 0;   /* Bit 7: 0=overlay active */
        md->fdc.side = md->side;

        /* Drive select */
        uint8_t new_drive = (value >> 5) & 0x03;
        if (new_drive != md->drive) {
            microdisc_select_drive(md, new_drive);
        }
    }
}

void microdisc_set_disk(microdisc_t* md, uint8_t drive, uint8_t* data, uint32_t size,
                        uint8_t tracks, uint8_t sectors_per_track) {
    if (drive >= MICRODISC_MAX_DRIVES) return;
    bool media_changed = (md->disk_data[drive] != data);
    md->disk_data[drive] = data;
    md->disk_size[drive] = size;
    md->disk_tracks[drive] = tracks;
    md->disk_sectors[drive] = sectors_per_track;
    /* New media in the slot: its surface is pristine (damage belongs to
     * the floppy, not the drive). Re-pointing the SAME image (savestate
     * restore, write-back refresh) keeps the map. */
    if (media_changed)
        memset(&md->bad_map[drive], 0, sizeof(md->bad_map[drive]));
    if (drive == md->drive) {
        fdc_set_disk(&md->fdc, data, size);
        md->fdc.tracks = tracks;
        md->fdc.sectors_per_track = sectors_per_track;
        fdc_set_bad_map(&md->fdc, &md->bad_map[drive]);
    }
}

int microdisc_add_bad_sector(microdisc_t* md, uint8_t drive,
                             uint8_t side, uint8_t track, uint8_t sector) {
    if (drive >= MICRODISC_MAX_DRIVES) return -1;
    if (fdc_bad_map_add(&md->bad_map[drive], side, track, sector) != 0) return -1;
    microdisc_sync_bad_map(md, drive);
    return 0;
}

void microdisc_sync_bad_map(microdisc_t* md, uint8_t drive) {
    if (drive >= MICRODISC_MAX_DRIVES) return;
    if (drive == md->drive)
        fdc_set_bad_map(&md->fdc, &md->bad_map[drive]);
}

bool microdisc_load_rom(microdisc_t* md, const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size > 16384) {
        fclose(fp);
        return false;
    }

    md->diskrom_data = (uint8_t*)malloc((size_t)size);
    if (!md->diskrom_data) {
        fclose(fp);
        return false;
    }

    size_t rd = fread(md->diskrom_data, 1, (size_t)size, fp);
    fclose(fp);

    if (rd != (size_t)size) {
        free(md->diskrom_data);
        md->diskrom_data = NULL;
        return false;
    }

    md->diskrom_size = (uint32_t)size;
    return true;
}

void microdisc_cleanup(microdisc_t* md) {
    if (md->diskrom_data) {
        free(md->diskrom_data);
        md->diskrom_data = NULL;
    }
}
