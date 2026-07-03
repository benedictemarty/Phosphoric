/**
 * @file disk.c
 * @brief FDC WD1793 disk controller emulation
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-23
 * @version 1.0.0-beta.7
 *
 * Implements the WD1793 FDC with timing-accurate DRQ/INTRQ delays,
 * matching the Oricutron approach for Microdisc/Sedoric compatibility.
 *
 * Key differences from the previous instant model:
 * - DRQ is delayed after command and after each byte read
 * - INTRQ is delayed after command completion
 * - Type I commands set proper status bits (HEADL, PULSE, TRK0)
 * - fdc_ticktock() must be called with cycle count to advance timers
 */

#include "storage/disk.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Trace FDC sur stderr si FDC_TRACE est définie (évalué une seule fois) */
int fdc_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = (getenv("FDC_TRACE") != NULL);
    return enabled;
}

/* Default no-op callback */
static void fdc_dummy_cb(void* userdata) { (void)userdata; }

void fdc_init(fdc_t* fdc) {
    memset(fdc, 0, sizeof(fdc_t));
    fdc->tracks = 80;
    fdc->sectors_per_track = 17;
    fdc->di_status = -1;
    fdc->dd_status = -1;
    fdc->set_drq = fdc_dummy_cb;
    fdc->clr_drq = fdc_dummy_cb;
    fdc->set_intrq = fdc_dummy_cb;
    fdc->clr_intrq = fdc_dummy_cb;
}

void fdc_reset(fdc_t* fdc) {
    fdc->status = 0;
    fdc->track = 0;
    fdc->sector = 1;
    fdc->c_track = 0;
    fdc->c_sector = 0;
    fdc->currentop = FDC_OP_NONE;
    fdc->cur_sector_data = NULL;
    fdc->cur_offset = 0;
    fdc->wt_state = 0;
    fdc->wt_field_idx = 0;
    fdc->wt_sectors_done = 0;
    fdc->delayed_drq = 0;
    fdc->delayed_int = 0;
    fdc->di_status = -1;
    fdc->dd_status = -1;
}

void fdc_set_disk(fdc_t* fdc, uint8_t* data, uint32_t size) {
    fdc->disk_data = data;
    fdc->disk_size = size;
}

int fdc_bad_map_add(fdc_bad_map_t* map, uint8_t side, uint8_t track, uint8_t sector) {
    if (map->count >= FDC_MAX_BAD_SECTORS) return -1;
    map->entry[map->count].side = side;
    map->entry[map->count].track = track;
    map->entry[map->count].sector = sector;
    map->count++;
    return 0;
}

int fdc_add_bad_sector(fdc_t* fdc, uint8_t side, uint8_t track, uint8_t sector) {
    return fdc_bad_map_add(&fdc->bad, side, track, sector);
}

void fdc_set_bad_map(fdc_t* fdc, const fdc_bad_map_t* map) {
    if (map) fdc->bad = *map;
    else memset(&fdc->bad, 0, sizeof(fdc->bad));
}

/**
 * Get pointer to sector data within flat disk image.
 * Layout: side * (tracks * spt) + track * spt + (sector_id - 1)
 */
static uint8_t* fdc_find_sector(fdc_t* fdc, uint8_t sec_id) {
    if (!fdc->disk_data) return NULL;
    if (fdc->c_track >= fdc->tracks || sec_id == 0 || sec_id > fdc->sectors_per_track)
        return NULL;
    /* Bad sector map: injected faults read as Record Not Found */
    for (uint8_t i = 0; i < fdc->bad.count; i++) {
        if (fdc->bad.entry[i].side == fdc->side &&
            fdc->bad.entry[i].track == fdc->c_track &&
            fdc->bad.entry[i].sector == sec_id)
            return NULL;
    }
    uint32_t offset = ((uint32_t)fdc->side * fdc->tracks * fdc->sectors_per_track +
                        (uint32_t)fdc->c_track * fdc->sectors_per_track +
                        (uint32_t)(sec_id - 1)) * 256;
    if (offset + 256 > fdc->disk_size) return NULL;
    return &fdc->disk_data[offset];
}

/**
 * Seek to specified track (Type I commands).
 * Sets delayed INTRQ and proper status bits.
 */
static void fdc_seek_track(fdc_t* fdc, uint8_t target) {
    if (fdc_trace_enabled()) {
        fprintf(stderr, "[FDC] seek target=%u (c_track=%u track_reg=%u data=%02X)\n",
                target, fdc->c_track, fdc->track, fdc->data);
    }
    if (fdc->disk_data) {
        if (target >= fdc->tracks) {
            target = (fdc->tracks > 0) ? fdc->tracks - 1 : 0;
            fdc->di_status = FDC_STI_HEADL | FDC_STI_SEEK_ERR;
        } else {
            fdc->di_status = FDC_STI_HEADL | FDC_STI_PULSE;
        }
        fdc->c_track = target;
        fdc->c_sector = 0;
        fdc->track = target;
        /* INTRQ fires after 20 cycles (much faster than real hardware) */
        fdc->delayed_int = 20;
        if (fdc->c_track == 0) fdc->di_status |= FDC_STI_TRK0;
    } else {
        /* No disk: immediate INTRQ with error */
        fdc->set_intrq(fdc->intrq_userdata);
        fdc->track = 0;
        fdc->status = FDC_ST_NOT_READY | FDC_STI_SEEK_ERR;
    }
}

/**
 * Process FDC timer ticks. Must be called with CPU cycle count
 * after each instruction to handle delayed DRQ/INTRQ.
 */
void fdc_ticktock(fdc_t* fdc, unsigned int cycles) {
    /* Delayed INTRQ */
    if (fdc->delayed_int > 0) {
        fdc->delayed_int -= cycles;
        if (fdc->delayed_int <= 0) {
            fdc->delayed_int = 0;
            if (fdc->di_status != -1) {
                fdc->status = (uint8_t)fdc->di_status;
                fdc->di_status = -1;
            }
            fdc->set_intrq(fdc->intrq_userdata);
        }
    }

    /* Delayed DRQ */
    if (fdc->delayed_drq > 0) {
        fdc->delayed_drq -= cycles;
        if (fdc->delayed_drq <= 0) {
            fdc->delayed_drq = 0;
            if (fdc->dd_status != -1) {
                fdc->status = (uint8_t)fdc->dd_status;
                fdc->dd_status = -1;
            }
            fdc->status |= FDC_ST_DRQ;
            fdc->set_drq(fdc->drq_userdata);
        }
    }
}

uint8_t fdc_read(fdc_t* fdc, uint8_t reg) {
    switch (reg & 3) {
    case FDC_STATUS:
        /* Reading status clears INTRQ */
        fdc->clr_intrq(fdc->intrq_userdata);
        return fdc->status;

    case FDC_TRACK:
        return fdc->track;

    case FDC_SECTOR:
        return fdc->sector;

    case FDC_DATA:
        switch (fdc->currentop) {
        case FDC_OP_READ_SECTOR:
        case FDC_OP_READ_SECTORS:
            if (!fdc->cur_sector_data) {
                fdc->status &= ~FDC_ST_DRQ;
                fdc->status |= FDC_ST_NOT_FOUND;
                fdc->clr_drq(fdc->drq_userdata);
                fdc->currentop = FDC_OP_NONE;
                break;
            }

            /* First read skips the data mark byte (0xFB) */
            if (fdc->cur_offset == 0) {
                fdc->sec_type = 0; /* Normal record */
            }

            /* Get the next byte */
            fdc->data = fdc->cur_sector_data[fdc->cur_offset++];

            /* Clear DRQ for this byte */
            fdc->status &= ~FDC_ST_DRQ;
            fdc->clr_drq(fdc->drq_userdata);

            /* Entire sector read? */
            if (fdc->cur_offset >= fdc->cur_sector_len) {
                if (fdc->currentop == FDC_OP_READ_SECTORS) {
                    /* Multi-sector: move to next */
                    fdc->sector++;
                    fdc->cur_offset = 0;
                    fdc->cur_sector_data = fdc_find_sector(fdc, fdc->sector);
                    if (!fdc->cur_sector_data) {
                        /* End of track */
                        fdc->delayed_int = 20;
                        fdc->di_status = fdc->sec_type;
                        fdc->currentop = FDC_OP_NONE;
                        fdc->status &= ~FDC_ST_DRQ;
                        fdc->clr_drq(fdc->drq_userdata);
                        break;
                    }
                    fdc->delayed_drq = 180;
                } else {
                    /* Single sector done */
                    fdc->delayed_int = 32;
                    fdc->di_status = fdc->sec_type;
                    fdc->currentop = FDC_OP_NONE;
                    fdc->status &= ~FDC_ST_DRQ;
                    fdc->clr_drq(fdc->drq_userdata);
                }
            } else {
                /* More data: DRQ again after delay */
                fdc->delayed_drq = 32;
            }
            break;

        case FDC_OP_READ_ADDRESS:
            /* Returns 6 bytes: FE, track, side, sector, size, CRC(2) */
            if (fdc->cur_sector_data) {
                /* Synthesize address field from current track/sector */
                uint8_t addr_field[6];
                addr_field[0] = fdc->c_track;    /* Track */
                addr_field[1] = fdc->side;       /* Side */
                addr_field[2] = fdc->sector;     /* Sector */
                addr_field[3] = 1;               /* Size code (1 = 256 bytes) */
                addr_field[4] = 0;               /* CRC hi */
                addr_field[5] = 0;               /* CRC lo */
                if (fdc->cur_offset == 0) fdc->sector = addr_field[2];
                fdc->data = addr_field[fdc->cur_offset++];
            }
            fdc->status &= ~FDC_ST_DRQ;
            fdc->clr_drq(fdc->drq_userdata);
            if (fdc->cur_offset >= 6) {
                fdc->delayed_int = 20;
                fdc->di_status = 0;
                fdc->currentop = FDC_OP_NONE;
            } else {
                fdc->delayed_drq = 32;
            }
            break;

        default:
            break;
        }
        return fdc->data;
    }
    return 0xFF;
}

void fdc_write(fdc_t* fdc, uint8_t reg, uint8_t value) {
    switch (reg & 3) {
    case FDC_COMMAND:
        /* Writing command clears INTRQ */
        fdc->clr_intrq(fdc->intrq_userdata);

        switch (value & 0xE0) {
        case 0x00: /* Restore or Seek */
            fdc->status = FDC_ST_BUSY;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            if ((value & 0x10) == 0x00) {
                /* Restore (Type I) */
                fdc_seek_track(fdc, 0);
            } else {
                /* Seek (Type I) */
                fdc_seek_track(fdc, fdc->data);
            }
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x20: /* Step (Type I) */
            fdc->status = FDC_ST_BUSY;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            if (fdc->direction == 0)
                fdc_seek_track(fdc, fdc->c_track + 1);
            else
                fdc_seek_track(fdc, fdc->c_track > 0 ? fdc->c_track - 1 : 0);
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x40: /* Step-in (Type I) */
            fdc->status = FDC_ST_BUSY;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            fdc->direction = 0;
            fdc_seek_track(fdc, fdc->c_track + 1);
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x60: /* Step-out (Type I) */
            fdc->status = FDC_ST_BUSY;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            fdc->direction = 1;
            if (fdc->c_track > 0)
                fdc_seek_track(fdc, fdc->c_track - 1);
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x80: /* Read sector (Type II) */
            fdc->cur_offset = 0;
            fdc->cur_sector_data = fdc_find_sector(fdc, fdc->sector);
            if (fdc_trace_enabled()) {
                fprintf(stderr, "[FDC] READ c_track=%u sector=%u side=%u %s\n",
                        fdc->c_track, fdc->sector, fdc->side,
                        fdc->cur_sector_data ? "ok" : "NOT_FOUND");
            }
            if (!fdc->cur_sector_data) {
                fdc->status = FDC_ST_NOT_FOUND;
                fdc->clr_drq(fdc->drq_userdata);
                fdc->set_intrq(fdc->intrq_userdata);
                fdc->currentop = FDC_OP_NONE;
                break;
            }
            fdc->cur_sector_len = 256; /* Size code 1 */
            fdc->status = FDC_ST_BUSY | FDC_ST_NOT_READY;
            fdc->delayed_drq = 60; /* First DRQ after 60 cycles */
            fdc->currentop = (value & 0x10) ? FDC_OP_READ_SECTORS : FDC_OP_READ_SECTOR;
            break;

        case 0xA0: /* Write sector (Type II) */
            fdc->cur_offset = 0;
            fdc->cur_sector_data = fdc_find_sector(fdc, fdc->sector);
            if (!fdc->cur_sector_data) {
                fdc->status = FDC_ST_NOT_FOUND;
                fdc->clr_drq(fdc->drq_userdata);
                fdc->set_intrq(fdc->intrq_userdata);
                fdc->currentop = FDC_OP_NONE;
                break;
            }
            fdc->cur_sector_len = 256;
            fdc->status = FDC_ST_BUSY | FDC_ST_NOT_READY;
            fdc->delayed_drq = 500; /* Write DRQ delay */
            fdc->currentop = (value & 0x10) ? FDC_OP_WRITE_SECTORS : FDC_OP_WRITE_SECTOR;
            break;

        case 0xC0: /* Read Address / Force Interrupt */
            if ((value & 0x10) == 0x00) {
                /* Read Address (Type III) */
                fdc->cur_offset = 0;
                /* Use first available sector on current track */
                fdc->cur_sector_data = fdc_find_sector(fdc, 1);
                if (!fdc->cur_sector_data) {
                    fdc->status = FDC_ST_NOT_FOUND;
                    fdc->clr_drq(fdc->drq_userdata);
                    fdc->set_intrq(fdc->intrq_userdata);
                    fdc->currentop = FDC_OP_NONE;
                    break;
                }
                fdc->status = FDC_ST_NOT_READY | FDC_ST_BUSY | FDC_ST_DRQ;
                fdc->set_drq(fdc->drq_userdata);
                fdc->currentop = FDC_OP_READ_ADDRESS;
            } else {
                /* Force Interrupt (Type IV) */
                fdc->status = 0;
                fdc->clr_drq(fdc->drq_userdata);
                fdc->set_intrq(fdc->intrq_userdata);
                fdc->delayed_int = 0;
                fdc->delayed_drq = 0;
                fdc->currentop = FDC_OP_NONE;
            }
            break;

        case 0xE0: /* Read Track / Write Track */
            if ((value & 0x10) == 0x00) {
                /* Read Track (Type III) - not fully implemented */
                fdc->status = FDC_ST_BUSY | FDC_ST_NOT_READY;
                fdc->delayed_drq = 60;
                fdc->currentop = FDC_OP_READ_TRACK;
            } else {
                /* Write Track (Type III) - track formatting. The ROM streams a
                 * raw IBM/MFM track; fdc_write() DATA parses it into sectors. */
                fdc->status = FDC_ST_NOT_READY | FDC_ST_BUSY;
                fdc->delayed_drq = 500;
                fdc->clr_drq(fdc->drq_userdata);
                fdc->clr_intrq(fdc->intrq_userdata);
                fdc->delayed_int = 0;
                fdc->wt_state = 0;
                fdc->wt_field_idx = 0;
                fdc->wt_data_len = 0;
                fdc->wt_sectors_done = 0;
                fdc->cur_sector_data = NULL;
                fdc->currentop = FDC_OP_WRITE_TRACK;
            }
            break;
        }
        break;

    case FDC_TRACK:
        fdc->track = value;
        break;

    case FDC_SECTOR:
        fdc->sector = value;
        break;

    case FDC_DATA:
        fdc->data = value;
        switch (fdc->currentop) {
        case FDC_OP_WRITE_SECTOR:
        case FDC_OP_WRITE_SECTORS:
            if (!fdc->cur_sector_data) {
                fdc->status &= ~FDC_ST_DRQ;
                fdc->status |= FDC_ST_NOT_FOUND;
                fdc->clr_drq(fdc->drq_userdata);
                fdc->currentop = FDC_OP_NONE;
                break;
            }
            fdc->cur_sector_data[fdc->cur_offset++] = value;
            fdc->disk_modified = true;   /* in-memory image mutated → drive dirty */
            fdc->status &= ~FDC_ST_DRQ;
            fdc->clr_drq(fdc->drq_userdata);

            if (fdc->cur_offset >= fdc->cur_sector_len) {
                if (fdc->currentop == FDC_OP_WRITE_SECTORS) {
                    fdc->sector++;
                    fdc->cur_offset = 0;
                    fdc->cur_sector_data = fdc_find_sector(fdc, fdc->sector);
                    if (!fdc->cur_sector_data) {
                        fdc->delayed_int = 20;
                        fdc->di_status = fdc->sec_type;
                        fdc->currentop = FDC_OP_NONE;
                        fdc->status &= ~FDC_ST_DRQ;
                        fdc->clr_drq(fdc->drq_userdata);
                        break;
                    }
                    fdc->delayed_drq = 180;
                } else {
                    fdc->delayed_int = 32;
                    fdc->di_status = fdc->sec_type;
                    fdc->currentop = FDC_OP_NONE;
                    fdc->status &= ~FDC_ST_DRQ;
                    fdc->clr_drq(fdc->drq_userdata);
                }
            } else {
                fdc->delayed_drq = 32;
            }
            break;

        case FDC_OP_WRITE_TRACK: {
            /* Parse the raw IBM/MFM format stream. Only address marks matter:
             *   0xFE = ID Address Mark    → next 4 bytes: track, side, sector, size
             *   0xFB / 0xF8 = Data AM     → next `len` bytes: the sector payload
             * Gap (0x4E), sync (0x00) and CRC-control bytes (0xF5/0xF6/0xF7/0xFC)
             * are layout only. 0xFE/0xFB occurring *inside* a data field are real
             * data and are never re-read as marks (that is what the state guards).
             * Completion: after one data field per sector on the track. */
            fdc->disk_modified = true;
            switch (fdc->wt_state) {
            case 0:  /* scanning for an address mark */
                if (value == 0xFE) {
                    fdc->wt_state = 1;          /* → collect the 4-byte ID field */
                    fdc->wt_field_idx = 0;
                } else if (value == 0xFB || value == 0xF8) {
                    /* Drop the upcoming data field into the sector the last ID
                     * named (logical position in the flat image). */
                    fdc->cur_sector_data = fdc_find_sector(fdc, fdc->wt_id[2]);
                    fdc->wt_data_len = (uint16_t)(128u << (fdc->wt_id[3] & 3));
                    if (fdc->wt_data_len == 0 || fdc->wt_data_len > 1024)
                        fdc->wt_data_len = 256;
                    fdc->cur_offset = 0;
                    fdc->wt_state = 2;          /* → collect the data field */
                }
                break;
            case 1:  /* collecting the 4-byte ID field */
                fdc->wt_id[fdc->wt_field_idx++] = value;
                if (fdc->wt_field_idx >= 4) fdc->wt_state = 0;
                break;
            case 2:  /* collecting the sector data field */
                if (fdc->cur_sector_data && fdc->cur_offset < fdc->wt_data_len)
                    fdc->cur_sector_data[fdc->cur_offset] = value;
                fdc->cur_offset++;             /* advance even if no sector (stay in sync) */
                if (fdc->cur_offset >= fdc->wt_data_len) {
                    fdc->wt_state = 0;
                    if (fdc->wt_sectors_done < 0xFF) fdc->wt_sectors_done++;
                }
                break;
            }
            fdc->status &= ~FDC_ST_DRQ;
            fdc->clr_drq(fdc->drq_userdata);

            if (fdc->sectors_per_track > 0 &&
                fdc->wt_sectors_done >= fdc->sectors_per_track) {
                /* Whole track formatted: finish (trailing gap bytes ignored). */
                fdc->delayed_int = 32;
                fdc->di_status = 0;
                fdc->currentop = FDC_OP_NONE;
            } else {
                fdc->delayed_drq = 32;          /* request the next byte */
            }
            break;
        }

        default:
            break;
        }
        break;
    }
}
