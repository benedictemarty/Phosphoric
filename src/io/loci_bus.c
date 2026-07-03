/**
 * @file loci_bus.c
 * @brief LOCI bus-facing peripherals — TAP cassette ($0315-$0317) and
 *        DSK WD1793 FDC ($0310-$031F).
 *
 * Sprint 34c R4 : mechanical split of loci.c.
 */

#include "io/loci.h"
#include "io/loci_internal.h"
#include "storage/sedoric.h"
#include "utils/logging.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ─── TAP cassette (Sprint 34af) ──────────────────────────────── */

bool loci_tap_open(loci_t* loci, const char* host_path) {
    if (loci->tap_fp) {
        fclose((FILE*)loci->tap_fp);
        loci->tap_fp = NULL;
    }
    /* Firmware REC writes into the mounted file — try read/write first. */
    bool wprot = false;
    FILE* fp = fopen(host_path, "r+b");
    if (!fp) {
        fp = fopen(host_path, "rb");
        wprot = true;
    }
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return false; }
    loci->tap_fp = fp;
    loci->tap_size = (uint32_t)sz;
    loci->tap_counter = 0;
    loci->tap_stat = 0;
    loci->tap_wprot = wprot;
    loci->tap_lead_in = 0;
    loci->tap_bit_counter = 0;
    loci->tap_data = 0;
    return true;
}

void loci_tap_close(loci_t* loci) {
    if (loci->tap_fp) {
        fclose((FILE*)loci->tap_fp);
        loci->tap_fp = NULL;
    }
    loci->tap_size = 0;
    loci->tap_counter = 0;
    loci->tap_stat = LOCI_TAP_STAT_NOT_READY;
}

/* ─── Low-level TAP protocol (Sprint 36f, firmware tap.c semantics) ──
 * PLAY/REC/READ_BIT are motor-protected: when the motor is off the
 * command stays latched in $0315 and executes once the motor turns on
 * (snooped from VIA ORB bit 6 via loci_tap_motor). REW/FFW always run.
 * Each completed command resets $0315 to 0x00 — that is the completion
 * handshake the locirom patches poll. */

static uint8_t tap_parity_odd(uint8_t byte) {
    uint8_t parity = 1;
    for (uint8_t i = 0; i < 8; i++) {
        if ((byte >> i) & 0x01) parity++;
    }
    return parity & 0x01;
}

static uint16_t tap_encode_frame(uint8_t byte) {
    /* 14-bit frame, LSB first: start 0, 8 data bits, odd parity, stops. */
    return (uint16_t)(0x3C00 | (tap_parity_odd(byte) << 9) | (byte << 1));
}

static uint8_t tap_read_byte_host(loci_t* loci) {
    if (loci->tap_lead_in < 8) {
        /* Synthetic lead-in after mount/rewind — counter untouched. */
        loci->tap_lead_in++;
        return 0x16;
    }
    FILE* fp = (FILE*)loci->tap_fp;
    if (fseek(fp, (long)loci->tap_counter, SEEK_SET) != 0) return 0x00;
    int c = fgetc(fp);
    if (c == EOF) return 0x00;   /* firmware returns 0 past EOF */
    loci->tap_counter++;
    return (uint8_t)c;
}

static void tap_exec_cmd(loci_t* loci, uint8_t cmd) {
    FILE* fp = (FILE*)loci->tap_fp;
    switch (cmd) {
        case LOCI_TAP_CMD_REW:
            if (fp) {
                fseek(fp, 0, SEEK_SET);
                loci->tap_counter = 0;
                loci->tap_bit_counter = 0;
                loci->tap_lead_in = 0;
            }
            loci->tap_cmd = 0;
            break;
        case LOCI_TAP_CMD_FFW:
            if (fp) {
                fseek(fp, 0, SEEK_END);
                loci->tap_counter = loci->tap_size;
            }
            loci->tap_cmd = 0;
            break;
        case LOCI_TAP_CMD_PLAY:
            if (!loci->tap_motor_on) return;          /* stays latched */
            loci->tap_data = fp ? tap_read_byte_host(loci) : 0x00;
            loci->tap_cmd = 0;
            break;
        case LOCI_TAP_CMD_READ_BIT:
            if (!loci->tap_motor_on) return;
            if (loci->tap_bit_counter == 0) {
                uint8_t b = fp ? tap_read_byte_host(loci) : 0x00;
                loci->tap_encoded = tap_encode_frame(b);
            }
            loci->tap_data = (uint8_t)((loci->tap_encoded >> loci->tap_bit_counter) & 0x01);
            if (++loci->tap_bit_counter >= 14) loci->tap_bit_counter = 0;
            loci->tap_cmd = 0;
            break;
        case LOCI_TAP_CMD_REC:
            if (!loci->tap_motor_on) return;
            if (fp && !loci->tap_wprot) {
                if (fseek(fp, (long)loci->tap_counter, SEEK_SET) == 0 &&
                    fputc(loci->tap_data, fp) != EOF) {
                    loci->tap_counter++;
                    if (loci->tap_counter > loci->tap_size) {
                        loci->tap_size = loci->tap_counter;
                    }
                    fflush(fp);
                }
            }
            loci->tap_cmd = 0;
            break;
        default:
            loci->tap_cmd = 0;
            break;
    }
}

void loci_tap_motor(loci_t* loci, bool on) {
    if (!loci || !loci->enabled) return;
    loci->tap_motor_on = on;
    /* Firmware: a motor-protected command latched while the motor was
     * off runs as soon as the state machine reaches TAP_READY. */
    if (on && loci->tap_cmd != 0) {
        tap_exec_cmd(loci, loci->tap_cmd);
    }
}

uint8_t loci_tap_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    switch (address) {
        case LOCI_TAP_IO_CMD:  return loci->tap_cmd;
        case LOCI_TAP_IO_STAT: {
            if (!loci->tap_fp) return LOCI_TAP_STAT_NOT_READY;
            uint8_t s = loci->tap_stat;
            if (loci->tap_wprot) s |= LOCI_TAP_STAT_WPROT;
            return s;
        }
        case LOCI_TAP_IO_DATA:
            return loci->tap_data;
    }
    return 0xFF;
}

void loci_tap_write(loci_t* loci, uint16_t address, uint8_t value) {
    if (!loci || !loci->enabled) return;
    if (address == LOCI_TAP_IO_CMD) {
        loci->tap_cmd = value;
        tap_exec_cmd(loci, value);
    } else if (address == LOCI_TAP_IO_DATA) {
        loci->tap_data = value;
    }
}

void op_tap_seek(loci_t* loci) {
    if (!loci->tap_fp) {
        api_return_errno(loci, LOCI_ENODEV);
        return;
    }
    uint32_t pos = (uint32_t)loci->regs[LOCI_REG_API_A]
                 | ((uint32_t)loci->regs[LOCI_REG_API_X]    <<  8)
                 | ((uint32_t)loci->regs[LOCI_REG_API_SREG] << 16)
                 | ((uint32_t)loci->regs[LOCI_REG_API_SREG_HI] << 24);
    if (loci->tap_size > 0 && pos >= loci->tap_size) {
        pos = loci->tap_size - 1;
    }
    if (fseek((FILE*)loci->tap_fp, (long)pos, SEEK_SET) != 0) {
        api_return_errno(loci, LOCI_EIO);
        return;
    }
    loci->tap_counter = pos;
    if (pos == 0) loci->tap_lead_in = 0;   /* firmware tap_seek() */
    api_return_axsreg(loci, pos);
}

void op_tap_tell(loci_t* loci) {
    api_return_axsreg(loci, loci->tap_counter);
}

void op_tap_read_header(loci_t* loci) {
    if (!loci->tap_fp) {
        api_return_errno(loci, LOCI_ENODEV);
        return;
    }
    if (loci->tap_size < LOCI_TAP_HEADER_SIZE + 4) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }

    FILE* fp = (FILE*)loci->tap_fp;
    fseek(fp, (long)loci->tap_counter, SEEK_SET);

    uint8_t w[4] = {0};
    int filled = 0;
    long sync_end = -1;
    long pos = (long)loci->tap_counter;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = (uint8_t)c;
        pos++;
        if (filled < 4) { filled++; continue; }
        if (w[0] == 0x16 && w[1] == 0x16 && w[2] == 0x16 && w[3] == 0x24) {
            sync_end = pos;
            break;
        }
    }
    if (sync_end < 0) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }

    uint8_t header[LOCI_TAP_HEADER_SIZE] = {0};
    size_t hr = fread(header, 1, LOCI_TAP_HEADER_SIZE, fp);
    if (hr != LOCI_TAP_HEADER_SIZE) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }
    for (int i = 9; i < LOCI_TAP_HEADER_SIZE; i++) {
        uint8_t ch = header[i];
        if (ch != 0 && (ch < 32 || ch >= 128)) header[i] = '?';
    }
    loci->tap_counter = (uint32_t)(sync_end + LOCI_TAP_HEADER_SIZE);

    xstack_zero(loci);
    loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - LOCI_TAP_HEADER_SIZE);
    memcpy(&loci->xstack[loci->xstack_ptr], header, LOCI_TAP_HEADER_SIZE);
    xstack_sync(loci);
    api_return_axsreg(loci, (uint32_t)(sync_end - 4));
}

/* ─── DSK WD1793 cycle-accurate (Sprint 34aw) ──────────────────── */

/* DRQ / INTRQ callbacks bridging fdc_t → loci.dsk_*. */
void loci_fdc_set_drq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (l) l->dsk_drq = 0x00;
}
void loci_fdc_clr_drq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (l) l->dsk_drq = 0x80;
}
void loci_fdc_set_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x00;
    if (l->dsk_intena && l->dsk_cpu_irq_set) {
        l->dsk_cpu_irq_set(l->dsk_bus_ctx);
    }
}
void loci_fdc_clr_intrq(void* userdata) {
    loci_t* l = (loci_t*)userdata;
    if (!l) return;
    l->dsk_intrq = 0x80;
    if (l->dsk_cpu_irq_clr) {
        l->dsk_cpu_irq_clr(l->dsk_bus_ctx);
    }
}

static void loci_apply_dsk_selection(loci_t* loci) {
    uint8_t drv = loci->dsk_selected;
    if (drv >= 4) return;
    fdc_set_disk(&loci->dsk_fdc,
                 loci->dsk_image[drv],
                 loci->dsk_image_size[drv]);
    loci->dsk_fdc.tracks            = loci->dsk_tracks[drv];
    loci->dsk_fdc.sectors_per_track = loci->dsk_sectors[drv];
    /* Damage follows the media: load this drive's media map */
    fdc_set_bad_map(&loci->dsk_fdc, &loci->dsk_bad_map[drv]);
}

int loci_add_bad_sector(loci_t* loci, uint8_t drive,
                        uint8_t side, uint8_t track, uint8_t sector) {
    if (drive >= 4) return -1;
    if (fdc_bad_map_add(&loci->dsk_bad_cli[drive], side, track, sector) != 0)
        return -1;
    /* Also damage the media currently mounted (if any) */
    fdc_bad_map_add(&loci->dsk_bad_map[drive], side, track, sector);
    if (loci->dsk_selected == drive)
        fdc_set_bad_map(&loci->dsk_fdc, &loci->dsk_bad_map[drive]);
    return 0;
}

bool loci_dsk_open(loci_t* loci, uint8_t drive, const char* host_path) {
    if (drive >= 4) return false;
    if (loci->dsk_fp[drive]) {
        fclose((FILE*)loci->dsk_fp[drive]);
        loci->dsk_fp[drive] = NULL;
    }
    if (loci->dsk_image[drive]) {
        free(loci->dsk_image[drive]);
        loci->dsk_image[drive] = NULL;
        loci->dsk_image_size[drive] = 0;
    }
    size_t pn = strlen(host_path);
    if (pn >= sizeof(loci->dsk_host_path[drive])) pn = sizeof(loci->dsk_host_path[drive]) - 1;
    memcpy(loci->dsk_host_path[drive], host_path, pn);
    loci->dsk_host_path[drive][pn] = '\0';

    sedoric_disk_t* sd = sedoric_load(host_path);
    if (sd) {
        loci->dsk_image[drive]      = sd->data;
        loci->dsk_image_size[drive] = sd->size;
        loci->dsk_tracks[drive]     = sd->tracks;
        loci->dsk_sectors[drive]    = sd->sectors;
        free(sd);
        loci->dsk_fp[drive] = fopen(host_path, "r+b");
        if (!loci->dsk_fp[drive]) loci->dsk_fp[drive] = fopen(host_path, "rb");
        FILE* probe = fopen(host_path, "rb");
        char hdr[8] = {0};
        if (probe) {
            (void)!fread(hdr, 1, 8, probe);
            fclose(probe);
        }
        loci->dsk_is_mfm[drive] = (memcmp(hdr, "MFM_DISK", 8) == 0);
        if (loci->dsk_is_mfm[drive]) {
            log_warning("LOCI dsk drive %d: MFM_DISK format — writes are session-only "
                     "(no MFM re-encoder)", drive);
        }
    } else {
        FILE* fp = fopen(host_path, "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > (long)(1 * 1024 * 1024)) { fclose(fp); return false; }
        uint8_t* buf = (uint8_t*)malloc((size_t)sz);
        if (!buf) { fclose(fp); return false; }
        if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
            free(buf); fclose(fp); return false;
        }
        loci->dsk_fp[drive]         = fp;
        loci->dsk_image[drive]      = buf;
        loci->dsk_image_size[drive] = (uint32_t)sz;
        loci->dsk_is_mfm[drive]     = false;
    }
    /* Fresh media in the slot: pristine surface, plus any CLI-injected
     * damage destined for this drive (--bad-sector parsed before mounts). */
    loci->dsk_bad_map[drive] = loci->dsk_bad_cli[drive];
    log_info("LOCI loci_dsk_open drive %d: %s (%u bytes, %d tracks, %d sectors)",
             drive, host_path, loci->dsk_image_size[drive],
             loci->dsk_tracks[drive], loci->dsk_sectors[drive]);
    if (loci->dsk_selected == drive) {
        loci_apply_dsk_selection(loci);
    }
    return true;
}

void loci_dsk_flush(loci_t* loci, uint8_t drive) {
    if (drive >= 4) return;
    if (loci->dsk_is_mfm[drive]) return;
    if (!loci->dsk_image[drive]) return;
    if (!loci->dsk_host_path[drive][0]) return;
    FILE* out = fopen(loci->dsk_host_path[drive], "wb");
    if (!out) {
        log_warning("LOCI dsk drive %d: cannot open '%s' for write-back",
                 drive, loci->dsk_host_path[drive]);
        return;
    }
    size_t n = fwrite(loci->dsk_image[drive], 1,
                      loci->dsk_image_size[drive], out);
    fclose(out);
    if (n != loci->dsk_image_size[drive]) {
        log_warning("LOCI dsk drive %d: short write %zu/%u to '%s'",
                 drive, n, loci->dsk_image_size[drive],
                 loci->dsk_host_path[drive]);
    } else {
        log_info("LOCI dsk drive %d: %u bytes flushed to '%s'",
                 drive, loci->dsk_image_size[drive],
                 loci->dsk_host_path[drive]);
    }
}

void loci_dsk_close(loci_t* loci, uint8_t drive) {
    if (drive >= 4) return;
    loci_dsk_flush(loci, drive);
    if (loci->dsk_fp[drive]) {
        fclose((FILE*)loci->dsk_fp[drive]);
        loci->dsk_fp[drive] = NULL;
    }
    if (loci->dsk_image[drive]) {
        free(loci->dsk_image[drive]);
        loci->dsk_image[drive] = NULL;
        loci->dsk_image_size[drive] = 0;
    }
    loci->dsk_host_path[drive][0] = '\0';
    loci->dsk_is_mfm[drive] = false;
    memset(&loci->dsk_bad_map[drive], 0, sizeof(loci->dsk_bad_map[drive]));
    if (loci->dsk_selected == drive) {
        fdc_set_disk(&loci->dsk_fdc, NULL, 0);
        fdc_set_bad_map(&loci->dsk_fdc, NULL);
    }
}

uint8_t loci_dsk_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    switch (address) {
        case LOCI_DSK_IO_CMD: {
            uint8_t s = fdc_read(&loci->dsk_fdc, 0);
            if (loci->dsk_selected < 4 && !loci->dsk_image[loci->dsk_selected]) {
                s |= LOCI_DSK_STAT_NOT_READY;
            }
            return s;
        }
        case LOCI_DSK_IO_TRACK: return fdc_read(&loci->dsk_fdc, 1);
        case LOCI_DSK_IO_SECT:  return fdc_read(&loci->dsk_fdc, 2);
        case LOCI_DSK_IO_DATA:  return fdc_read(&loci->dsk_fdc, 3);
        /* Firmware serves exact 0x80 (idle) / 0x00 (asserted) bytes for
         * the active-low IRQ and DRQ registers — no |0x7F padding. */
        case LOCI_DSK_IO_CTRL:  return loci->dsk_intrq;
        case LOCI_DSK_IO_DRQ:   return loci->dsk_drq;
        case LOCI_DSK_IO_ID:    return 'L';
        case LOCI_DSK_IO_SPARE0: return loci->dsk_spare[0];
        case LOCI_DSK_IO_SPARE1: return loci->dsk_spare[1];
    }
    return 0xFF;
}

void loci_dsk_write(loci_t* loci, uint16_t address, uint8_t value) {
    if (!loci || !loci->enabled) return;
    switch (address) {
        case LOCI_DSK_IO_CMD:   fdc_write(&loci->dsk_fdc, 0, value); break;
        case LOCI_DSK_IO_TRACK: fdc_write(&loci->dsk_fdc, 1, value); break;
        case LOCI_DSK_IO_SECT:  fdc_write(&loci->dsk_fdc, 2, value); break;
        case LOCI_DSK_IO_DATA:  fdc_write(&loci->dsk_fdc, 3, value); break;
        case LOCI_DSK_IO_CTRL: {
            loci->dsk_ctrl = value;
            loci->dsk_intena = (value & 0x01) != 0;
            uint8_t side = (value & 0x10) ? 1 : 0;
            bool diskrom = (value & 0x80) == 0;
            bool romdis = (value & 0x02) == 0;
            uint8_t newdrv = (uint8_t)((value & LOCI_DSK_CTRL_DRV_SEL_MASK)
                                       >> LOCI_DSK_CTRL_DRV_SEL_SHIFT);
            loci->dsk_fdc.side = side;
            if (newdrv != loci->dsk_selected) {
                loci->dsk_selected = newdrv;
                loci_apply_dsk_selection(loci);
            }
            if (loci->dsk_sync_overlay) {
                loci->dsk_sync_overlay(loci->dsk_bus_ctx, romdis, diskrom);
            }
            if (loci->dsk_intena && loci->dsk_intrq == 0x00) {
                if (loci->dsk_cpu_irq_set) loci->dsk_cpu_irq_set(loci->dsk_bus_ctx);
            } else {
                if (loci->dsk_cpu_irq_clr) loci->dsk_cpu_irq_clr(loci->dsk_bus_ctx);
            }
            break;
        }
        case LOCI_DSK_IO_DRQ:   loci->dsk_drq = value; break;
        case LOCI_DSK_IO_SPARE0: loci->dsk_spare[0] = value; break;
        case LOCI_DSK_IO_SPARE1: loci->dsk_spare[1] = value; break;
    }
}
