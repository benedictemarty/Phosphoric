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

    /* Current operation */
    fdc_op_t currentop;

    /* Sector data pointers (into disk_data, like Oricutron's cached sectors) */
    uint8_t* cur_sector_data;  /* Pointer to current sector's 256 bytes */
    uint16_t cur_sector_len;   /* Sector size (256 for size code 1) */
    uint16_t cur_offset;       /* Current byte offset within sector */
    uint8_t sec_type;          /* Record type (0 or FDC_ST_REC_TYPE for deleted) */

    /* Delayed DRQ/INTRQ (timing model) */
    int delayed_drq;           /* Cycles until DRQ asserts (0 = no pending) */
    int delayed_int;           /* Cycles until INTRQ asserts (0 = no pending) */
    int di_status;             /* Status to set when delayed_int fires (-1 = keep) */
    int dd_status;             /* Status to set when delayed_drq fires (-1 = keep) */

    /* Signal callbacks */
    fdc_signal_cb set_drq;
    fdc_signal_cb clr_drq;
    void* drq_userdata;
    fdc_signal_cb set_intrq;
    fdc_signal_cb clr_intrq;
    void* intrq_userdata;
} fdc_t;

void fdc_init(fdc_t* fdc);
void fdc_reset(fdc_t* fdc);
void fdc_set_disk(fdc_t* fdc, uint8_t* data, uint32_t size);
uint8_t fdc_read(fdc_t* fdc, uint8_t reg);
void fdc_write(fdc_t* fdc, uint8_t reg, uint8_t value);
void fdc_ticktock(fdc_t* fdc, unsigned int cycles);

/* Trace FDC sur stderr si la variable d'env FDC_TRACE est définie (debug) */
int fdc_trace_enabled(void);

#endif /* DISK_H */
