/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file disk.h
 * @brief FDC WD1793 disk controller emulation - public API
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-23
 * @version 1.0.0-beta.7
 *
 * Emulates the WD1793 FDC with timing-accurate DRQ/INTRQ delays,
 * matching the Oricutron approach for Microdisc compatibility.
 */

#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

/* FDC Registers */
#define FDC_STATUS  0
#define FDC_COMMAND 0
#define FDC_TRACK   1
#define FDC_SECTOR  2
#define FDC_DATA    3

/* Status bits - Type I commands */
#define FDC_ST_BUSY      0x01
#define FDC_STI_PULSE    0x02  /* Index pulse (Type I only) */
#define FDC_ST_DRQ       0x02  /* Data Request (Type II/III) */
#define FDC_STI_TRK0     0x04  /* Track 0 (Type I only) */
#define FDC_ST_LOST_DATA 0x04  /* Lost data (Type II/III) */
#define FDC_ST_CRC_ERROR 0x08
#define FDC_STI_SEEK_ERR 0x10  /* Seek error (Type I) */
#define FDC_ST_NOT_FOUND 0x10  /* Record not found (Type II/III) */
#define FDC_STI_HEADL    0x20  /* Head loaded (Type I) */
#define FDC_ST_REC_TYPE  0x20  /* Record type / deleted mark (Type II read) */
#define FDC_ST_WRITE_PROT 0x40
#define FDC_ST_NOT_READY 0x80

/* Timing profile.
 * FAST = legacy short delays (Oricutron-style). Kept for the LOCI dsk_fdc:
 *        on real hardware the LOCI's "drive" is the RP2040 serving sectors
 *        from SD, with no mechanics — near-instant seeks are the faithful
 *        behaviour there.
 * REAL = mechanical 3" drive on a 1 MHz WD1793 (Microdisc): step rates
 *        6/12/20/30 ms per track, 300 RPM rotation (200 ms/rev), first DRQ
 *        after the requested sector actually passes under the head, Record
 *        Not Found only after 5 index pulses (~1 s), 30 ms head settling
 *        when the command's E/V flag asks for it, and a live index pulse
 *        in the Type I status. */
#define FDC_TIMING_FAST 0
#define FDC_TIMING_REAL 1

#define FDC_REV_CYCLES         200000u  /* one revolution at 300 RPM, 1 MHz */
/* Duration of one byte at 250 kbit/s in MFM (double density): 32 µs, i.e. 32 cycles
 * at 1 MHz. This is the budget the CPU has to service each DRQ. */
#define FDC_BYTE_CYCLES        32
#define FDC_INDEX_PULSE_CYCLES 4000u    /* index pulse width (~4 ms) */
#define FDC_SETTLE_CYCLES      30000    /* E/V flag: 30 ms at 1 MHz clock */
#define FDC_RNF_CYCLES         (5 * (int)FDC_REV_CYCLES) /* 5 index pulses */

/* Current operation */
typedef enum {
    FDC_OP_NONE = 0,
    FDC_OP_READ_SECTOR,
    FDC_OP_READ_SECTORS,
    FDC_OP_WRITE_SECTOR,
    FDC_OP_WRITE_SECTORS,
    FDC_OP_READ_ADDRESS,
    FDC_OP_READ_TRACK,
    FDC_OP_WRITE_TRACK
} fdc_op_t;

/* Callback types for DRQ/INTRQ notification */
typedef void (*fdc_signal_cb)(void* userdata);

/* Bad sector map: unreadable sectors of ONE media (one floppy).
 * Lives with the media image at the controller layer; the FDC holds a
 * copy for the media currently under the head. */
#define FDC_MAX_BAD_SECTORS 16
typedef struct {
    struct {
        uint8_t side, track, sector;
    } entry[FDC_MAX_BAD_SECTORS];
    uint8_t count;
} fdc_bad_map_t;

typedef struct fdc_s {
    uint8_t status;
    uint8_t command;
    uint8_t track;         /* Track register (r_track) */
    uint8_t sector;        /* Sector register (r_sector) */
    uint8_t data;          /* Data register (r_data) */
    uint8_t direction;     /* 0 = step in, 1 = step out */

    /* Internal state */
    uint8_t c_track;       /* Current physical track */
    uint8_t c_sector;      /* Current sector index in cached track */
    uint8_t side;          /* Current side (0 or 1) */

    /* Disk data pointer (from sedoric) */
    uint8_t* disk_data;
    uint32_t disk_size;
    uint8_t tracks;
    uint8_t sectors_per_track;

    /* Set true whenever a sector/track write mutates disk_data. The Microdisc
     * layer consumes this to mark the current drive dirty (for .dsk write-back).
     * Cleared by the consumer; the FDC only ever sets it. */
    bool disk_modified;

    /* Current operation */
    fdc_op_t currentop;

    /* Sector data pointers (into disk_data, like Oricutron's cached sectors) */
    uint8_t* cur_sector_data;  /* Pointer to current sector's 256 bytes */
    uint16_t cur_sector_len;   /* Sector size (256 for size code 1) */
    uint16_t cur_offset;       /* Current byte offset within sector */
    uint8_t sec_type;          /* Record type (0 or FDC_ST_REC_TYPE for deleted) */

    /* Write Track (formatting) stream parser. The ROM streams a raw IBM/MFM
     * track byte-by-byte via the DATA register; we recognise ID Address Marks
     * (0xFE + track/side/sector/size) and Data Address Marks (0xFB/0xF8 + sector
     * bytes) and drop the data fields into the flat image. Gap/sync/CRC-control
     * bytes are layout only. */
    uint8_t  wt_state;         /* 0=scan marks, 1=ID field, 2=data field */
    uint8_t  wt_field_idx;     /* bytes collected so far in the ID field */
    uint8_t  wt_id[4];         /* last ID field: track, side, sector, size code */
    uint16_t wt_data_len;      /* expected bytes in the current data field */
    uint8_t  wt_sectors_done;  /* data fields completed this track (→ completion) */

    /* Delayed DRQ/INTRQ (timing model) */
    int delayed_drq;           /* Cycles until DRQ asserts (0 = no pending) */
    int delayed_int;           /* Cycles until INTRQ asserts (0 = no pending) */
    int di_status;             /* Status to set when delayed_int fires (-1 = keep) */
    int dd_status;             /* Status to set when delayed_drq fires (-1 = keep) */

    /* Mechanical timing model (FDC_TIMING_REAL) */
    uint8_t  timing_mode;      /* FDC_TIMING_FAST (default) or FDC_TIMING_REAL */
    uint32_t rot_pos;          /* disk angle in cycles, 0..FDC_REV_CYCLES-1 */

    /* Count of lost bytes (S2 LOST DATA) since the last reset. A healthy
     * transfer must stay at zero: this is a diagnostic indicator,
     * not a hardware state. */
    uint32_t lost_data_count;

    /* Write-protect tab (S6). On a real drive this is a mechanical
     * sensor: the controller rejects any write command and raises
     * bit 6 of the status. Wired by fdc_set_write_protect() — on a
     * read-only .dsk file, or by --disk-write-protect. */
    bool write_protected;

    /* Age of the current DRQ, in cycles. Beyond one byte time, the data is
     * considered lost (S2). Reset each time DRQ is raised. */
    int drq_age;
    bool     status_type1;     /* status register shows Type I bits (live
                                  index pulse / TRK0 patched on read) */

    /* Signal callbacks */
    fdc_signal_cb set_drq;
    fdc_signal_cb clr_drq;
    void* drq_userdata;
    fdc_signal_cb set_intrq;
    fdc_signal_cb clr_intrq;
    void* intrq_userdata;

    /* Web-backed media (loci-webdisk archi B): the flat image starts empty and
     * raw 6400-byte MFM tracks are fetched from an HTTP disk server on demand
     * (per track, on first sector access), then extracted into disk_data.
     * web=false → ordinary in-RAM disk (behaviour unchanged). */
    bool     web;                       /* sectors served by HTTP on demand */
    char     web_url[512];              /* base disk URL (http:// only) */
    uint8_t  web_track_loaded[2 * 82];  /* 1 once (side*tracks+track) fetched */

    /* Bad sector map of the media currently under the head (fault injection
     * for robustness testing). Sectors listed here become invisible to
     * fdc_find_sector -> the command layer reports Record Not Found
     * (FDC_ST_NOT_FOUND), like a physically damaged disk. Matches real
     * WD1793 behaviour on unreadable sectors; the flat-image model
     * otherwise cannot express it (a corrupted MFM ID field is silently
     * healed at load time). Damage belongs to the MEDIA, not the drive:
     * the controller layers (Microdisc, LOCI) keep one map per drive slot
     * and swap it in through fdc_set_bad_map() on drive select / insert. */
    fdc_bad_map_t bad;
} fdc_t;

void fdc_init(fdc_t* fdc);
void fdc_reset(fdc_t* fdc);
void fdc_set_disk(fdc_t* fdc, uint8_t* data, uint32_t size);

/**
 * @brief Sets or removes the write-protect tab (S6)
 *
 * Write Sector / Write Track commands are then rejected, without modifying
 * anything, with the WRITE PROTECT bit and an interrupt — like the WD1793.
 */
void fdc_set_write_protect(fdc_t* fdc, bool protect);
/* loci-webdisk (archi B): make the current media web-backed (raw MFM tracks
 * fetched over HTTP on demand). Call after fdc_set_disk() with a zeroed flat
 * image of the right geometry; url="" or NULL disables web backing. */
void fdc_set_web(fdc_t* fdc, const char* url);
/* Mark side/track/sector (sector is 1-based) as unreadable in MAP.
 * Returns 0 on success, -1 if the map is full. */
int fdc_bad_map_add(fdc_bad_map_t* map, uint8_t side, uint8_t track, uint8_t sector);
/* Same, directly on the media currently loaded in the FDC. */
int fdc_add_bad_sector(fdc_t* fdc, uint8_t side, uint8_t track, uint8_t sector);
/* Load MAP as the current media's bad sector map (copied; NULL = pristine). */
void fdc_set_bad_map(fdc_t* fdc, const fdc_bad_map_t* map);
uint8_t fdc_read(fdc_t* fdc, uint8_t reg);
void fdc_write(fdc_t* fdc, uint8_t reg, uint8_t value);
void fdc_ticktock(fdc_t* fdc, unsigned int cycles);

/* État sérialisable du WD1793/WD177x (sections .ost « FDC » et « JAS ») : registres,
 * opération en cours, modèle mécanique, âge du DRQ et analyseur de formatage. Les
 * pointeurs vers les images disque ne sont PAS écrits : le contrôleur les
 * re-pointe après chargement, puis appelle fdc_state_resume() qui recalcule le
 * pointeur du secteur en cours. FDC_STATE_SIZE = taille écrite (v2) ;
 * fdc_state_load lit `size` octets : un .ost antérieur (36 octets, ou 31 avant
 * v1.42) se relit, les champs absents prenant leur valeur neutre. */
#define FDC_STATE_SIZE_V1 36
#define FDC_STATE_SIZE    49
void fdc_state_save(const fdc_t* fdc, FILE* fp);
void fdc_state_load(fdc_t* fdc, FILE* fp, uint32_t size);
void fdc_state_resume(fdc_t* fdc);

/* FDC trace on stderr if the env variable FDC_TRACE is set (debug) */
int fdc_trace_enabled(void);

#endif /* DISK_H */
