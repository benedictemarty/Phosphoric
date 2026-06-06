/* tests/unit/test_loci_sdimg.c — Sprint 34ao
 *
 * Unit tests for the SD raw image backend. Generates a minimal FAT16
 * superfloppy image on the fly (no binary fixtures committed) and
 * exercises open/read/seek/closedir/readdir against it. */
#define _POSIX_C_SOURCE 200809L

#include "io/loci_sdimg.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do {                                                       \
    printf("  [%d] %-50s ", tests_passed + tests_failed + 1, #name);         \
    int prev_fail = tests_failed;                                            \
    test_##name();                                                           \
    if (tests_failed == prev_fail) { printf("PASS\n"); tests_passed++; }     \
    else printf("FAIL\n");                                                   \
} while (0)

#define ASSERT_TRUE(cond) do {                                               \
    if (!(cond)) {                                                           \
        printf("\n    ASSERT_TRUE failed: %s (line %d)", #cond, __LINE__);   \
        tests_failed++; return;                                              \
    }                                                                        \
} while (0)

#define ASSERT_EQ(a, b) do {                                                 \
    long _a = (long)(a), _b = (long)(b);                                     \
    if (_a != _b) {                                                          \
        printf("\n    ASSERT_EQ failed: %ld != %ld (line %d)",               \
               _a, _b, __LINE__);                                            \
        tests_failed++; return;                                              \
    }                                                                        \
} while (0)

/* ─── FAT16 image generator ──────────────────────────────────────── */

#define BPS 512
#define SPC 1
#define RSV 1
#define NF  2
#define RE  64                /* root entries */
#define FATSZ 32              /* sectors per FAT — covers >4085 clusters */
#define TOTAL_SEC 8000        /* gives ~4100 data clusters → FAT16 */

static const char* g_img_path = NULL;

static void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* Write an 8.3 dir entry into the buffer at `idx`. */
static void make_entry(uint8_t* root_sec, int idx,
                       const char* name83, uint8_t attrib,
                       uint16_t first_cluster, uint32_t size) {
    uint8_t* e = root_sec + idx * 32;
    memset(e, ' ', 11);
    /* name83 is "NAMEEXT" up to 11 chars, space-padded. */
    size_t nlen = strlen(name83);
    if (nlen > 11) nlen = 11;
    for (size_t i = 0; i < nlen; i++) e[i] = (uint8_t)name83[i];
    e[11] = attrib;
    put_u16(e + 20, 0);                   /* hi cluster (FAT16) */
    put_u16(e + 26, first_cluster);
    put_u32(e + 28, size);
}

/* Build a FAT16 image, place a file "HELLO   TXT" with cluster #2
 * containing "Hello, LOCI!\n" (13 bytes), and a directory "SUB" at
 * cluster #3 with one file "INSIDE  BIN" of 4 bytes at cluster #4. */
static const char* create_test_image(void) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/loci_sdimg_test_%d.img", (int)getpid());
    FILE* fp = fopen(path, "wb");
    if (!fp) return NULL;

    uint8_t sector[BPS];
    memset(sector, 0, sizeof(sector));

    /* === Sector 0: BPB === */
    /* BS_jmpBoot (3) + BS_OEMName (8) */
    sector[0] = 0xEB; sector[1] = 0x3C; sector[2] = 0x90;
    memcpy(sector + 3, "PHOSPHRC", 8);
    put_u16(sector + 11, BPS);
    sector[13] = SPC;
    put_u16(sector + 14, RSV);
    sector[16] = NF;
    put_u16(sector + 17, RE);
    put_u16(sector + 19, TOTAL_SEC);  /* <65536 → fits in 16 bits */
    sector[21] = 0xF8;                /* media type */
    put_u16(sector + 22, FATSZ);
    /* Signature 0xAA55 at offsets 510/511 */
    sector[510] = 0x55; sector[511] = 0xAA;
    fwrite(sector, 1, BPS, fp);

    /* === Reserved area done. Now FATs. === */
    uint8_t* fat = calloc(FATSZ * BPS, 1);
    /* FAT[0] = media descriptor + 0xFF padding (FAT16: 0xFFF8 then 0xFFFF). */
    put_u16(fat + 0, 0xFFF8);
    put_u16(fat + 2, 0xFFFF);
    /* Cluster 2 (HELLO.TXT): single cluster → EOC. */
    put_u16(fat + 2 * 2, 0xFFFF);
    /* Cluster 3 (SUB dir): single cluster → EOC. */
    put_u16(fat + 3 * 2, 0xFFFF);
    /* Cluster 4 (INSIDE.BIN): single cluster → EOC. */
    put_u16(fat + 4 * 2, 0xFFFF);

    for (int i = 0; i < NF; i++) fwrite(fat, 1, FATSZ * BPS, fp);
    free(fat);

    /* === Root directory ===
     * sectors: RE * 32 / BPS = 64*32/512 = 4 sectors. */
    uint8_t* root = calloc(RE * 32, 1);
    make_entry(root, 0, "HELLO   TXT", 0x20, 2, 13);    /* archive */
    make_entry(root, 1, "SUB        ", 0x10, 3, 0);      /* dir */
    make_entry(root, 2, "VOLUME  LBL", 0x08, 0, 0);      /* volume — ignored */
    /* (free entries beyond stay zero → terminate dir scan) */
    fwrite(root, 1, RE * 32, fp);
    free(root);

    /* === Data area === */
    /* Cluster 2 = HELLO.TXT contents */
    memset(sector, 0, BPS);
    memcpy(sector, "Hello, LOCI!\n", 13);
    fwrite(sector, 1, BPS, fp);

    /* Cluster 3 = SUB directory: one entry INSIDE.BIN */
    uint8_t sub_dir[BPS];
    memset(sub_dir, 0, sizeof(sub_dir));
    make_entry(sub_dir, 0, "INSIDE  BIN", 0x20, 4, 4);
    fwrite(sub_dir, 1, BPS, fp);

    /* Cluster 4 = INSIDE.BIN contents (4 bytes) */
    memset(sector, 0, BPS);
    sector[0] = 0xDE; sector[1] = 0xAD; sector[2] = 0xBE; sector[3] = 0xEF;
    fwrite(sector, 1, BPS, fp);

    /* Pad image to TOTAL_SEC sectors. */
    memset(sector, 0, BPS);
    long pos = ftell(fp);
    long want = (long)TOTAL_SEC * BPS;
    while (pos < want) {
        fwrite(sector, 1, BPS, fp);
        pos += BPS;
    }
    fclose(fp);
    return path;
}

static void cleanup_test_image(void) {
    if (g_img_path) unlink(g_img_path);
}

/* ─── Tests ──────────────────────────────────────────────────────── */

TEST(open_image_detects_fat16) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    ASSERT_TRUE(img != NULL);
    ASSERT_TRUE(strcmp(loci_sdimg_fs_label(img), "FAT16") == 0);
    ASSERT_TRUE(loci_sdimg_total_size(img) == (uint32_t)TOTAL_SEC * BPS);
    loci_sdimg_close(img);
}

TEST(open_nonexistent_fails) {
    loci_sdimg_t* img = loci_sdimg_open("/tmp/does_not_exist_loci.img");
    ASSERT_TRUE(img == NULL);
}

TEST(opendir_root_lists_entries) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    ASSERT_TRUE(img != NULL);
    int dh = loci_sdimg_opendir(img, "");
    ASSERT_TRUE(dh >= 0);
    char name[64];
    uint8_t attrib;
    uint32_t size;
    int found_hello = 0, found_sub = 0;
    while (loci_sdimg_readdir(img, dh, name, &attrib, &size) == 1) {
        if (strcmp(name, "HELLO.TXT") == 0) {
            found_hello = 1;
            ASSERT_EQ(size, 13);
            ASSERT_TRUE((attrib & 0x10) == 0);  /* not a dir */
        } else if (strcmp(name, "SUB") == 0) {
            found_sub = 1;
            ASSERT_TRUE((attrib & 0x10) != 0);  /* is dir */
        }
    }
    ASSERT_TRUE(found_hello && found_sub);
    loci_sdimg_closedir(img, dh);
    loci_sdimg_close(img);
}

TEST(fopen_read_hello) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    ASSERT_TRUE(img != NULL);
    int fd = loci_sdimg_fopen(img, "HELLO.TXT");
    ASSERT_TRUE(fd >= 0);
    char buf[32] = {0};
    int br = loci_sdimg_fread(img, fd, buf, 13);
    ASSERT_EQ(br, 13);
    ASSERT_TRUE(memcmp(buf, "Hello, LOCI!\n", 13) == 0);
    /* Read past EOF → 0 */
    br = loci_sdimg_fread(img, fd, buf, 16);
    ASSERT_EQ(br, 0);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);
}

TEST(fopen_case_insensitive) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    int fd = loci_sdimg_fopen(img, "hello.txt");
    ASSERT_TRUE(fd >= 0);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);
}

TEST(fopen_nested_path) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    int fd = loci_sdimg_fopen(img, "SUB/INSIDE.BIN");
    ASSERT_TRUE(fd >= 0);
    uint8_t buf[8] = {0};
    int br = loci_sdimg_fread(img, fd, buf, 4);
    ASSERT_EQ(br, 4);
    ASSERT_EQ(buf[0], 0xDE);
    ASSERT_EQ(buf[1], 0xAD);
    ASSERT_EQ(buf[2], 0xBE);
    ASSERT_EQ(buf[3], 0xEF);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);
}

TEST(fopen_missing_returns_enoent) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    int fd = loci_sdimg_fopen(img, "NOPE.XYZ");
    ASSERT_TRUE(fd < 0);
    ASSERT_EQ(-fd, ENOENT);
    loci_sdimg_close(img);
}

TEST(lseek_set_cur_end) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    int fd = loci_sdimg_fopen(img, "HELLO.TXT");
    ASSERT_TRUE(fd >= 0);

    int32_t pos = loci_sdimg_lseek(img, fd, 7, 0);   /* SET */
    ASSERT_EQ(pos, 7);
    char c;
    int br = loci_sdimg_fread(img, fd, &c, 1);
    ASSERT_EQ(br, 1);
    ASSERT_EQ((uint8_t)c, 'L');

    pos = loci_sdimg_lseek(img, fd, 0, 2);            /* END */
    ASSERT_EQ(pos, 13);

    pos = loci_sdimg_lseek(img, fd, -1, 1);           /* CUR */
    ASSERT_EQ(pos, 12);

    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);
}

TEST(opendir_subdir_lists_inside) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    int dh = loci_sdimg_opendir(img, "SUB");
    ASSERT_TRUE(dh >= 0);
    char name[64];
    uint8_t attrib;
    uint32_t size;
    int r = loci_sdimg_readdir(img, dh, name, &attrib, &size);
    ASSERT_EQ(r, 1);
    ASSERT_TRUE(strcmp(name, "INSIDE.BIN") == 0);
    ASSERT_EQ(size, 4);
    /* Next entry → end of dir */
    r = loci_sdimg_readdir(img, dh, name, &attrib, &size);
    ASSERT_EQ(r, 0);
    loci_sdimg_closedir(img, dh);
    loci_sdimg_close(img);
}

TEST(fopen_bad_handle_close) {
    loci_sdimg_t* img = loci_sdimg_open(g_img_path);
    int r = loci_sdimg_fclose(img, 99);
    ASSERT_TRUE(r < 0);
    ASSERT_EQ(-r, EBADF);
    loci_sdimg_close(img);
}

int main(void) {
    printf("\n=== LOCI SD raw image backend tests (Sprint 34ao) ===\n");

    g_img_path = create_test_image();
    if (!g_img_path) {
        fprintf(stderr, "ERROR: failed to create test image\n");
        return 1;
    }
    printf("Test image: %s\n\n", g_img_path);

    RUN(open_image_detects_fat16);
    RUN(open_nonexistent_fails);
    RUN(opendir_root_lists_entries);
    RUN(fopen_read_hello);
    RUN(fopen_case_insensitive);
    RUN(fopen_nested_path);
    RUN(fopen_missing_returns_enoent);
    RUN(lseek_set_cur_end);
    RUN(opendir_subdir_lists_inside);
    RUN(fopen_bad_handle_close);

    cleanup_test_image();

    printf("\n===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("===========================================================\n");
    return tests_failed == 0 ? 0 : 1;
}
