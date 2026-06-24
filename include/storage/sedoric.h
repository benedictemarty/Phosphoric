/**
 * @file sedoric.h
 * @brief Sedoric disk filesystem - public API
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-23
 * @version 1.0.0-beta.6
 */

#ifndef SEDORIC_H
#define SEDORIC_H

#include <stdint.h>
#include <stdbool.h>

#define SEDORIC_SECTOR_SIZE   256
#define SEDORIC_TRACKS        42
#define SEDORIC_SECTORS       17
#define SEDORIC_SIDES         1
#define SEDORIC_DISK_SIZE     (SEDORIC_TRACKS * SEDORIC_SECTORS * SEDORIC_SECTOR_SIZE)

/* MFM_DISK format constants */
#define MFM_DISK_HEADER_SIZE  256
#define MFM_TRACK_SIZE        6400
#define MFM_MAX_TRACKS        82
#define MFM_MAX_SIDES         2
#define MFM_MAX_SECTORS       17
#define SEDORIC_DIR_TRACK     20
#define SEDORIC_DIR_SECTOR    4
#define SEDORIC_MAX_NAME      9
#define SEDORIC_MAX_EXT       3

typedef struct sedoric_disk_s {
    uint8_t* data;
    uint32_t size;
    uint8_t tracks;
    uint8_t sectors;
    uint8_t sides;
    bool modified;
    bool is_mfm;          /* Loaded from MFM_DISK format */
    uint8_t* mfm_raw;     /* Original MFM container kept for in-place write-back (NULL if raw) */
    uint32_t mfm_raw_size;
} sedoric_disk_t;

typedef struct sedoric_entry_s {
    char name[SEDORIC_MAX_NAME + 1];
    char ext[SEDORIC_MAX_EXT + 1];
    uint8_t start_track;
    uint8_t start_sector;
    uint16_t size;
    uint8_t type;
} sedoric_entry_t;

sedoric_disk_t* sedoric_create(void);
sedoric_disk_t* sedoric_load(const char* filename);
bool sedoric_save(sedoric_disk_t* disk, const char* filename);
void sedoric_destroy(sedoric_disk_t* disk);
uint8_t* sedoric_get_sector(sedoric_disk_t* disk, uint8_t track, uint8_t sector);
bool sedoric_read_sector(const sedoric_disk_t* disk, uint8_t track, uint8_t sector, uint8_t* buffer);
bool sedoric_write_sector(sedoric_disk_t* disk, uint8_t track, uint8_t sector, const uint8_t* buffer);

#endif /* SEDORIC_H */
