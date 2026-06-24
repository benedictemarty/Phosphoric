/**
 * @file test_storage.c
 * @brief Storage subsystem tests (Sedoric, FDC)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-23
 * @version 1.0.0-beta.7
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "storage/sedoric.h"
#include "storage/disk.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected 0x%X, got 0x%X\n", __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* Test signal tracking */
static bool test_drq_set = false;
static bool test_intrq_set = false;

static void test_set_drq(void* ud)   { (void)ud; test_drq_set = true; }
static void test_clr_drq(void* ud)   { (void)ud; test_drq_set = false; }
static void test_set_intrq(void* ud) { (void)ud; test_intrq_set = true; }
static void test_clr_intrq(void* ud) { (void)ud; test_intrq_set = false; }

static void fdc_init_test(fdc_t* fdc) {
    fdc_init(fdc);
    fdc->set_drq = test_set_drq;
    fdc->clr_drq = test_clr_drq;
    fdc->set_intrq = test_set_intrq;
    fdc->clr_intrq = test_clr_intrq;
    test_drq_set = false;
    test_intrq_set = false;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SEDORIC TESTS                                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_sedoric_create) {
    sedoric_disk_t* disk = sedoric_create();
    ASSERT_TRUE(disk != NULL);
    ASSERT_TRUE(disk->data != NULL);
    ASSERT_EQ(disk->tracks, 42);
    ASSERT_EQ(disk->sectors, 17);
    ASSERT_FALSE(disk->modified);
    sedoric_destroy(disk);
}

TEST(test_sedoric_sector_rw) {
    sedoric_disk_t* disk = sedoric_create();
    uint8_t write_buf[256];
    uint8_t read_buf[256];
    memset(write_buf, 0xAA, 256);

    ASSERT_TRUE(sedoric_write_sector(disk, 1, 1, write_buf));
    ASSERT_TRUE(disk->modified);

    memset(read_buf, 0, 256);
    ASSERT_TRUE(sedoric_read_sector(disk, 1, 1, read_buf));
    ASSERT_EQ(read_buf[0], 0xAA);
    ASSERT_EQ(read_buf[255], 0xAA);

    sedoric_destroy(disk);
}

TEST(test_sedoric_invalid_sector) {
    sedoric_disk_t* disk = sedoric_create();

    /* Sector 0 is invalid (1-based) */
    ASSERT_TRUE(sedoric_get_sector(disk, 0, 0) == NULL);
    /* Track out of range */
    ASSERT_TRUE(sedoric_get_sector(disk, 50, 1) == NULL);
    /* Sector out of range */
    ASSERT_TRUE(sedoric_get_sector(disk, 0, 20) == NULL);

    uint8_t buf[256];
    ASSERT_FALSE(sedoric_read_sector(disk, 50, 1, buf));

    sedoric_destroy(disk);
}

TEST(test_sedoric_system_info) {
    sedoric_disk_t* disk = sedoric_create();
    uint8_t buf[256];
    ASSERT_TRUE(sedoric_read_sector(disk, 0, 1, buf));
    ASSERT_EQ(buf[0], 'S');
    ASSERT_EQ(buf[1], 'E');
    ASSERT_EQ(buf[2], 'D');
    sedoric_destroy(disk);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  FDC TESTS                                                        */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_fdc_init) {
    fdc_t fdc;
    fdc_init(&fdc);
    ASSERT_EQ(fdc.tracks, 80);
    ASSERT_EQ(fdc.sectors_per_track, 17);
    ASSERT_EQ(fdc.currentop, FDC_OP_NONE);
}

TEST(test_fdc_reset) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    fdc.track = 10;
    fdc.sector = 5;
    fdc_reset(&fdc);
    ASSERT_EQ(fdc.track, 0);
    ASSERT_EQ(fdc.sector, 1);
    ASSERT_EQ(fdc.currentop, FDC_OP_NONE);
}

TEST(test_fdc_restore) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    /* Need disk data for successful restore */
    uint8_t* disk_data = calloc(80 * 17 * 256, 1);
    fdc_set_disk(&fdc, disk_data, 80 * 17 * 256);
    fdc.c_track = 20;
    fdc.track = 20;
    fdc_write(&fdc, 0, 0x00); /* Restore */
    ASSERT_EQ(fdc.track, 0);
    ASSERT_EQ(fdc.c_track, 0);
    /* INTRQ is delayed by 20 cycles */
    ASSERT_FALSE(test_intrq_set);
    fdc_ticktock(&fdc, 25);
    ASSERT_TRUE(test_intrq_set);
    /* Status should have TRK0 bit set */
    ASSERT_TRUE(fdc.status & FDC_STI_TRK0);
    free(disk_data);
}

TEST(test_fdc_seek) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    uint8_t* disk_data = calloc(80 * 17 * 256, 1);
    fdc_set_disk(&fdc, disk_data, 80 * 17 * 256);
    fdc_write(&fdc, 3, 15); /* DATA = 15 */
    fdc_write(&fdc, 0, 0x10); /* Seek */
    ASSERT_EQ(fdc.track, 15);
    ASSERT_EQ(fdc.c_track, 15);
    fdc_ticktock(&fdc, 25);
    ASSERT_TRUE(test_intrq_set);
    free(disk_data);
}

TEST(test_fdc_read_sector) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    uint8_t* disk_data = calloc(80 * 17 * 256, 1);
    disk_data[0] = 0xAA;
    disk_data[255] = 0xBB;
    fdc_set_disk(&fdc, disk_data, 80 * 17 * 256);

    fdc.c_track = 0;
    fdc.track = 0;
    fdc.sector = 1;
    fdc_write(&fdc, 0, 0x80); /* Read sector */
    ASSERT_EQ(fdc.currentop, FDC_OP_READ_SECTOR);

    /* DRQ is delayed by 60 cycles */
    ASSERT_FALSE(test_drq_set);
    fdc_ticktock(&fdc, 65);
    ASSERT_TRUE(test_drq_set);

    /* Read first byte */
    uint8_t first = fdc_read(&fdc, 3);
    ASSERT_EQ(first, 0xAA);
    ASSERT_FALSE(test_drq_set); /* DRQ cleared after read */

    /* Advance DRQ for remaining bytes */
    for (int i = 1; i < 255; i++) {
        fdc_ticktock(&fdc, 35); /* Let DRQ fire */
        fdc_read(&fdc, 3);
    }

    /* Read last byte */
    fdc_ticktock(&fdc, 35);
    uint8_t last = fdc_read(&fdc, 3);
    ASSERT_EQ(last, 0xBB);
    ASSERT_EQ(fdc.currentop, FDC_OP_NONE);

    /* INTRQ fires after delay */
    fdc_ticktock(&fdc, 35);
    ASSERT_TRUE(test_intrq_set);

    free(disk_data);
}

TEST(test_fdc_write_sector) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    uint8_t* disk_data = calloc(80 * 17 * 256, 1);
    fdc_set_disk(&fdc, disk_data, 80 * 17 * 256);

    fdc.c_track = 0;
    fdc.track = 0;
    fdc.sector = 1;
    fdc_write(&fdc, 0, 0xA0); /* Write sector */
    ASSERT_EQ(fdc.currentop, FDC_OP_WRITE_SECTOR);

    /* Wait for DRQ */
    fdc_ticktock(&fdc, 505);
    ASSERT_TRUE(test_drq_set);

    /* Write 256 bytes with DRQ delays between each */
    for (int i = 0; i < 256; i++) {
        fdc_write(&fdc, 3, (uint8_t)i);
        if (i < 255) fdc_ticktock(&fdc, 35);
    }
    ASSERT_EQ(fdc.currentop, FDC_OP_NONE);
    ASSERT_EQ(disk_data[0], 0x00);
    ASSERT_EQ(disk_data[1], 0x01);
    ASSERT_EQ(disk_data[255], 0xFF);
    free(disk_data);
}

TEST(test_fdc_force_interrupt) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    fdc.currentop = FDC_OP_READ_SECTOR;
    fdc_write(&fdc, 0, 0xD0); /* Force interrupt */
    ASSERT_EQ(fdc.currentop, FDC_OP_NONE);
    ASSERT_TRUE(test_intrq_set);
}

TEST(test_fdc_status_read_clears_intrq) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    test_intrq_set = true;
    fdc_read(&fdc, 0);
    ASSERT_FALSE(test_intrq_set);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                              */
/* ═══════════════════════════════════════════════════════════════════ */

/* A sector write through the FDC must flag the image as modified, so the
 * Microdisc can mark the drive dirty for .dsk write-back. */
TEST(test_fdc_write_sets_modified) {
    fdc_t fdc;
    fdc_init_test(&fdc);
    ASSERT_FALSE(fdc.disk_modified);
    uint8_t* disk_data = calloc(80 * 17 * 256, 1);
    fdc_set_disk(&fdc, disk_data, 80 * 17 * 256);

    fdc.c_track = 0;
    fdc.track = 0;
    fdc.sector = 1;
    fdc_write(&fdc, 0, 0xA0);          /* Write sector */
    fdc_ticktock(&fdc, 505);
    fdc_write(&fdc, 3, 0x42);          /* first data byte → mutates the image */
    ASSERT_TRUE(fdc.disk_modified);    /* flagged dirty */
    free(disk_data);
}

/* sedoric_save() must persist the in-memory image to disk so in-game saves
 * survive: write a sector, save, reload, verify the bytes round-trip. */
TEST(test_sedoric_save_roundtrip) {
    sedoric_disk_t* disk = sedoric_create();
    ASSERT_TRUE(disk != NULL);

    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(0xA0 ^ i);
    ASSERT_TRUE(sedoric_write_sector(disk, 5, 3, buf));

    const char* path = "test_storage_save.dsk";
    ASSERT_TRUE(sedoric_save(disk, path));
    sedoric_destroy(disk);

    sedoric_disk_t* reloaded = sedoric_load(path);
    ASSERT_TRUE(reloaded != NULL);
    uint8_t rd[256];
    ASSERT_TRUE(sedoric_read_sector(reloaded, 5, 3, rd));
    for (int i = 0; i < 256; i++) ASSERT_EQ(rd[i], (uint8_t)(0xA0 ^ i));
    sedoric_destroy(reloaded);
    remove(path);
}

/* Build a minimal one-track MFM_DISK image holding a single sector
 * (track 0, side 0, sector 1) and write it to `path`. */
static void make_minimal_mfm(const char* path) {
    uint8_t* img = calloc(MFM_DISK_HEADER_SIZE + MFM_TRACK_SIZE, 1);
    memcpy(img, "MFM_DISK", 8);
    img[8] = 1;   /* sides = 1 (LE uint32) */
    img[12] = 1;  /* tracks = 1 */
    uint8_t* trk = img + MFM_DISK_HEADER_SIZE;
    int p = 20;
    trk[p++] = 0xA1; trk[p++] = 0xA1; trk[p++] = 0xA1; trk[p++] = 0xFE; /* ID mark */
    trk[p++] = 0;    /* track */
    trk[p++] = 0;    /* side */
    trk[p++] = 1;    /* sector */
    trk[p++] = 1;    /* size code = 256 */
    trk[p++] = 0; trk[p++] = 0; /* ID CRC (ignored by loader) */
    p += 12;         /* gap */
    trk[p++] = 0xA1; trk[p++] = 0xA1; trk[p++] = 0xA1; trk[p++] = 0xFB; /* data mark */
    for (int k = 0; k < 256; k++) trk[p++] = 0x55;
    trk[p++] = 0; trk[p++] = 0; /* data CRC placeholder */
    FILE* fp = fopen(path, "wb");
    fwrite(img, 1, MFM_DISK_HEADER_SIZE + MFM_TRACK_SIZE, fp);
    fclose(fp);
    free(img);
}

/* Regression: writing back an MFM_DISK must re-inject sectors in place into
 * the MFM container (with valid CRC), NOT dump the flat buffer over it. */
TEST(test_sedoric_mfm_writeback_roundtrip) {
    const char* path = "test_storage_mfm.dsk";
    make_minimal_mfm(path);
    long mfm_size = MFM_DISK_HEADER_SIZE + MFM_TRACK_SIZE;

    sedoric_disk_t* disk = sedoric_load(path);
    ASSERT_TRUE(disk != NULL);
    ASSERT_TRUE(disk->is_mfm);
    ASSERT_TRUE(disk->mfm_raw != NULL);

    uint8_t* sec = sedoric_get_sector(disk, 0, 1);
    ASSERT_TRUE(sec != NULL);
    ASSERT_EQ(sec[0], 0x55);            /* extracted from MFM */
    sec[0] = 0xDE; sec[1] = 0xAD;
    ASSERT_TRUE(sedoric_save(disk, path));
    sedoric_destroy(disk);

    /* File must still be a valid MFM container of the same size (not a flat dump) */
    FILE* fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    fseek(fp, 0, SEEK_END);
    ASSERT_EQ((long)ftell(fp), mfm_size);
    fclose(fp);

    sedoric_disk_t* reloaded = sedoric_load(path);
    ASSERT_TRUE(reloaded != NULL);
    ASSERT_TRUE(reloaded->is_mfm);
    uint8_t* rsec = sedoric_get_sector(reloaded, 0, 1);
    ASSERT_TRUE(rsec != NULL);
    ASSERT_EQ(rsec[0], 0xDE);
    ASSERT_EQ(rsec[1], 0xAD);
    sedoric_destroy(reloaded);
    remove(path);
}

int main(void) {
    printf("Running Storage tests...\n");
    printf("═══════════════════════════════════════════════════════════\n");

    printf("\n  Sedoric:\n");
    RUN(test_sedoric_create);
    RUN(test_sedoric_sector_rw);
    RUN(test_sedoric_invalid_sector);
    RUN(test_sedoric_system_info);

    printf("\n  FDC WD1793:\n");
    RUN(test_fdc_init);
    RUN(test_fdc_reset);
    RUN(test_fdc_restore);
    RUN(test_fdc_seek);
    RUN(test_fdc_read_sector);
    RUN(test_fdc_write_sector);
    RUN(test_fdc_write_sets_modified);
    RUN(test_fdc_force_interrupt);
    RUN(test_fdc_status_read_clears_intrq);

    printf("\n  Disk persistence:\n");
    RUN(test_sedoric_save_roundtrip);
    RUN(test_sedoric_mfm_writeback_roundtrip);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
