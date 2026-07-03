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
    fdc->status_type1 = false;
    /* rot_pos is NOT reset: the platter keeps spinning across a reset */
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

/* Step rates of the WD1793 at 1 MHz clock, per command bits r1r0 (ms). */
static const uint16_t fdc_step_rate_ms[4] = {6, 12, 20, 30};

/* Cycles until SEC_ID's data field passes under the head (REAL model).
 * The track is modelled as sectors_per_track even angular slices, data
 * field sitting one eighth of a slice after the sector's ID field. */
static int fdc_rot_wait(fdc_t* fdc, uint8_t sec_id) {
    uint32_t spt = fdc->sectors_per_track ? fdc->sectors_per_track : 17;
    uint32_t slice = FDC_REV_CYCLES / spt;
    uint32_t target = ((uint32_t)(sec_id - 1) * slice + slice / 8) % FDC_REV_CYCLES;
    uint32_t wait = (target + FDC_REV_CYCLES - fdc->rot_pos) % FDC_REV_CYCLES;
    return (wait < 2) ? 2 : (int)wait;
}

/**
 * Seek to specified track (Type I commands).
 * Sets delayed INTRQ and proper status bits. CMD carries the command byte
 * (r1r0 step rate + V verify flag) for the REAL timing model.
 */
static void fdc_seek_track(fdc_t* fdc, uint8_t target, uint8_t cmd) {
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
        if (fdc->timing_mode == FDC_TIMING_REAL) {
            /* Head movement: one step-rate delay per track crossed (at
             * least one), plus 30 ms settling when V asks for a verify. */
            int steps = (target > fdc->c_track) ? target - fdc->c_track
                                                : fdc->c_track - target;
            if (steps == 0) steps = 1;
            fdc->delayed_int = steps * (int)fdc_step_rate_ms[cmd & 3] * 1000;
            if (cmd & 0x04) fdc->delayed_int += FDC_SETTLE_CYCLES;
        } else {
            /* FAST: INTRQ after 20 cycles (LOCI SD-served media) */
            fdc->delayed_int = 20;
        }
        fdc->c_track = target;
        fdc->c_sector = 0;
        fdc->track = target;
        if (fdc->c_track == 0) fdc->di_status |= FDC_STI_TRK0;
    } else {
        /* No disk: immediate INTRQ with error */
        fdc->set_intrq(fdc->intrq_userdata);
        fdc->track = 0;
        fdc->status = FDC_ST_NOT_READY | FDC_STI_SEEK_ERR;
    }
}

/* Sector not found (bad geometry or bad-sector map). On a real WD1793 the
 * controller keeps scanning ID fields and only gives up after 5 index
 * pulses (~1 s): stay BUSY and post RNF+INTRQ then. FAST reports at once. */
static void fdc_record_not_found(fdc_t* fdc) {
    fdc->clr_drq(fdc->drq_userdata);
    fdc->currentop = FDC_OP_NONE;
    if (fdc->timing_mode == FDC_TIMING_REAL) {
        fdc->status = FDC_ST_BUSY;
        fdc->di_status = FDC_ST_NOT_FOUND;
        fdc->delayed_int = FDC_RNF_CYCLES;
    } else {
        fdc->status = FDC_ST_NOT_FOUND;
        fdc->set_intrq(fdc->intrq_userdata);
    }
}

/**
 * Process FDC timer ticks. Must be called with CPU cycle count
 * after each instruction to handle delayed DRQ/INTRQ.
 */
void fdc_ticktock(fdc_t* fdc, unsigned int cycles) {
    /* The disk spins whether the controller is busy or not (300 RPM) */
    fdc->rot_pos = (fdc->rot_pos + cycles) % FDC_REV_CYCLES;

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
        /* REAL model: Type I status carries the LIVE index pulse (one per
         * revolution) and track-0 state, not a frozen snapshot. */
        if (fdc->timing_mode == FDC_TIMING_REAL && fdc->status_type1 &&
            !(fdc->status & FDC_ST_BUSY)) {
            uint8_t st = fdc->status & (uint8_t)~(FDC_STI_PULSE | FDC_STI_TRK0);
            if (fdc->disk_data && fdc->rot_pos < FDC_INDEX_PULSE_CYCLES)
                st |= FDC_STI_PULSE;
            if (fdc->c_track == 0)
                st |= FDC_STI_TRK0;
            return st;
        }
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
                    fdc->delayed_drq = (fdc->timing_mode == FDC_TIMING_REAL)
                                     ? fdc_rot_wait(fdc, fdc->sector) : 180;
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
            fdc->status_type1 = true;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            if ((value & 0x10) == 0x00) {
                /* Restore (Type I) */
                fdc_seek_track(fdc, 0, value);
            } else {
                /* Seek (Type I) */
                fdc_seek_track(fdc, fdc->data, value);
            }
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x20: /* Step (Type I) */
            fdc->status = FDC_ST_BUSY;
            fdc->status_type1 = true;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            if (fdc->direction == 0)
                fdc_seek_track(fdc, fdc->c_track + 1, value);
            else
                fdc_seek_track(fdc, fdc->c_track > 0 ? fdc->c_track - 1 : 0, value);
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x40: /* Step-in (Type I) */
            fdc->status = FDC_ST_BUSY;
            fdc->status_type1 = true;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            fdc->direction = 0;
            fdc_seek_track(fdc, fdc->c_track + 1, value);
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x60: /* Step-out (Type I) */
            fdc->status = FDC_ST_BUSY;
            fdc->status_type1 = true;
            if (value & 0x08) fdc->status |= FDC_STI_HEADL;
            fdc->direction = 1;
            if (fdc->c_track > 0)
                fdc_seek_track(fdc, fdc->c_track - 1, value);
            fdc->currentop = FDC_OP_NONE;
            break;

        case 0x80: /* Read sector (Type II) */
            fdc->status_type1 = false;
            fdc->cur_offset = 0;
            fdc->cur_sector_data = fdc_find_sector(fdc, fdc->sector);
            if (fdc_trace_enabled()) {
                fprintf(stderr, "[FDC] READ c_track=%u sector=%u side=%u %s\n",
                        fdc->c_track, fdc->sector, fdc->side,
                        fdc->cur_sector_data ? "ok" : "NOT_FOUND");
            }
            if (!fdc->cur_sector_data) {
                fdc_record_not_found(fdc);
                break;
            }
            fdc->cur_sector_len = 256; /* Size code 1 */
            fdc->status = FDC_ST_BUSY | FDC_ST_NOT_READY;
            if (fdc->timing_mode == FDC_TIMING_REAL) {
                /* First DRQ once the sector actually passes under the head
                 * (rotational latency), plus 30 ms settling if E is set. */
                fdc->delayed_drq = fdc_rot_wait(fdc, fdc->sector)
                                 + ((value & 0x04) ? FDC_SETTLE_CYCLES : 0);
            } else {
                fdc->delayed_drq = 60; /* First DRQ after 60 cycles */
            }
            fdc->currentop = (value & 0x10) ? FDC_OP_READ_SECTORS : FDC_OP_READ_SECTOR;
            break;

        case 0xA0: /* Write sector (Type II) */
            fdc->status_type1 = false;
            fdc->cur_offset = 0;
            fdc->cur_sector_data = fdc_find_sector(fdc, fdc->sector);
            if (!fdc->cur_sector_data) {
                fdc_record_not_found(fdc);
                break;
            }
            fdc->cur_sector_len = 256;
            fdc->status = FDC_ST_BUSY | FDC_ST_NOT_READY;
            if (fdc->timing_mode == FDC_TIMING_REAL) {
                fdc->delayed_drq = fdc_rot_wait(fdc, fdc->sector)
                                 + ((value & 0x04) ? FDC_SETTLE_CYCLES : 0);
            } else {
                fdc->delayed_drq = 500; /* Write DRQ delay */
            }
            fdc->currentop = (value & 0x10) ? FDC_OP_WRITE_SECTORS : FDC_OP_WRITE_SECTOR;
            break;

        case 0xC0: /* Read Address / Force Interrupt */
            if ((value & 0x10) == 0x00) {
                /* Read Address (Type III) */
                fdc->status_type1 = false;
                fdc->cur_offset = 0;
                /* Use first available sector on current track */
                fdc->cur_sector_data = fdc_find_sector(fdc, 1);
                if (!fdc->cur_sector_data) {
                    fdc_record_not_found(fdc);
                    break;
                }
                if (fdc->timing_mode == FDC_TIMING_REAL) {
                    /* First ID field reaches the head with the rotation */
                    fdc->status = FDC_ST_NOT_READY | FDC_ST_BUSY;
                    fdc->delayed_drq = fdc_rot_wait(fdc, 1);
                    fdc->currentop = FDC_OP_READ_ADDRESS;
                } else {
                    fdc->status = FDC_ST_NOT_READY | FDC_ST_BUSY | FDC_ST_DRQ;
                    fdc->set_drq(fdc->drq_userdata);
                    fdc->currentop = FDC_OP_READ_ADDRESS;
                }
            } else {
                /* Force Interrupt (Type IV). The WD1793 reloads the status
                 * register with Type I bits after a Force Interrupt. */
                fdc->status = 0;
                fdc->status_type1 = true;
                fdc->clr_drq(fdc->drq_userdata);
                fdc->set_intrq(fdc->intrq_userdata);
                fdc->delayed_int = 0;
                fdc->delayed_drq = 0;
                fdc->currentop = FDC_OP_NONE;
            }
            break;

        case 0xE0: /* Read Track / Write Track */
            fdc->status_type1 = false;
            if ((value & 0x10) == 0x00) {
                /* Read Track (Type III) - not fully implemented */
                fdc->status = FDC_ST_BUSY | FDC_ST_NOT_READY;
                fdc->delayed_drq = 60;
                fdc->currentop = FDC_OP_READ_TRACK;
            } else {
                /* Write Track (Type III) - track formatting. The ROM streams a
                 * raw IBM/MFM track; fdc_write() DATA parses it into sectors.
                 * On real hardware formatting starts at the index pulse. */
                fdc->status = FDC_ST_NOT_READY | FDC_ST_BUSY;
                fdc->delayed_drq = (fdc->timing_mode == FDC_TIMING_REAL)
                                 ? (int)(FDC_REV_CYCLES - fdc->rot_pos) : 500;
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
                    fdc->delayed_drq = (fdc->timing_mode == FDC_TIMING_REAL)
                                     ? fdc_rot_wait(fdc, fdc->sector) : 180;
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
