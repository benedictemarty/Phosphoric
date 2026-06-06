/* tests/unit/test_loci_sdimg_write.c — Sprint 34ap
 *
 * Write-path tests for the SDIMG backend. Builds a fresh FAT16 image
 * for each test, exercises the write API, then reopens it (separate
 * loci_sdimg_t) to verify integrity from a cold read. */
#define _POSIX_C_SOURCE 200809L

#include "io/loci_sdimg.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do {                                                       \
    printf("  [%d] %-55s ", tests_passed + tests_failed + 1, #name);         \
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

/* ─── FAT16 image generator (empty image, write tests fill it) ── */

#define BPS  512
#define SPC  1              /* 512 B clusters → crosses boundary quickly */
#define RSV  1
#define NF   2
#define RE   64
#define FATSZ 32
#define TOTAL_SEC 8000

static void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static char g_path[256];

static void create_blank_image(void) {
    snprintf(g_path, sizeof(g_path),
             "/tmp/loci_sdimg_w_%d.img", (int)getpid());
    FILE* fp = fopen(g_path, "wb");
    if (!fp) { perror(g_path); exit(1); }
    uint8_t sec[BPS] = {0};
    /* BPB */
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    memcpy(sec + 3, "PHOSPHRC", 8);
    put_u16(sec + 11, BPS);
    sec[13] = SPC;
    put_u16(sec + 14, RSV);
    sec[16] = NF;
    put_u16(sec + 17, RE);
    put_u16(sec + 19, TOTAL_SEC);
    sec[21] = 0xF8;
    put_u16(sec + 22, FATSZ);
    sec[510] = 0x55; sec[511] = 0xAA;
    fwrite(sec, 1, BPS, fp);

    /* FATs: media descriptor + 0xFFFF padding. */
    uint8_t* fat = calloc(FATSZ * BPS, 1);
    put_u16(fat + 0, 0xFFF8);
    put_u16(fat + 2, 0xFFFF);
    for (int i = 0; i < NF; i++) fwrite(fat, 1, FATSZ * BPS, fp);
    free(fat);

    /* Root dir + data area: all zero. */
    uint8_t zero[BPS] = {0};
    long want = (long)TOTAL_SEC * BPS;
    long pos = ftell(fp);
    while (pos < want) {
        fwrite(zero, 1, BPS, fp);
        pos += BPS;
    }
    fclose(fp);
}

static void destroy_image(void) {
    if (g_path[0]) unlink(g_path);
}

/* ─── Tests ──────────────────────────────────────────────────────── */

TEST(open_blank_image_is_writable) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    ASSERT_TRUE(img != NULL);
    ASSERT_TRUE(strcmp(loci_sdimg_fs_label(img), "FAT16") == 0);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(create_small_file_round_trip) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int fd = loci_sdimg_fopen_ex(img, "HELLO.TXT", 1);
    ASSERT_TRUE(fd >= 0);
    const char* msg = "Hello, FAT!";
    int bw = loci_sdimg_fwrite(img, fd, msg, (uint16_t)strlen(msg));
    ASSERT_EQ(bw, (int)strlen(msg));
    loci_sdimg_fclose(img, fd);
    loci_sdimg_sync(img);
    loci_sdimg_close(img);

    /* Reopen and verify. */
    img = loci_sdimg_open(g_path);
    fd = loci_sdimg_fopen(img, "HELLO.TXT");
    ASSERT_TRUE(fd >= 0);
    char buf[32] = {0};
    int br = loci_sdimg_fread(img, fd, buf, sizeof(buf));
    ASSERT_EQ(br, (int)strlen(msg));
    ASSERT_TRUE(memcmp(buf, msg, strlen(msg)) == 0);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(file_listed_in_root_after_create) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int fd = loci_sdimg_fopen_ex(img, "ONE.BIN", 1);
    uint8_t one = 0x42;
    loci_sdimg_fwrite(img, fd, &one, 1);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);

    img = loci_sdimg_open(g_path);
    int dh = loci_sdimg_opendir(img, "");
    char name[64]; uint8_t attrib; uint32_t size;
    int found = 0;
    while (loci_sdimg_readdir(img, dh, name, &attrib, &size) == 1) {
        if (strcmp(name, "ONE.BIN") == 0) { found = 1; ASSERT_EQ(size, 1); }
    }
    ASSERT_TRUE(found);
    loci_sdimg_closedir(img, dh);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(write_across_cluster_boundary) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int fd = loci_sdimg_fopen_ex(img, "BIG.BIN", 1);
    /* 5000 bytes > 1 cluster (2KB) → exercises chain extension. */
    uint8_t pat[100];
    for (size_t i = 0; i < sizeof(pat); i++) pat[i] = (uint8_t)(i & 0xFF);
    int total = 0;
    for (int i = 0; i < 50; i++) {
        int n = loci_sdimg_fwrite(img, fd, pat, sizeof(pat));
        ASSERT_EQ(n, (int)sizeof(pat));
        total += n;
    }
    ASSERT_EQ(total, 5000);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);

    img = loci_sdimg_open(g_path);
    fd = loci_sdimg_fopen(img, "BIG.BIN");
    uint8_t out[5000];
    int br = 0;
    while (br < 5000) {
        int n = loci_sdimg_fread(img, fd, out + br, 100);
        if (n <= 0) break;
        br += n;
    }
    ASSERT_EQ(br, 5000);
    for (int i = 0; i < 5000; i++) {
        ASSERT_EQ(out[i], (uint8_t)(i % 100));
    }
    loci_sdimg_close(img);
    destroy_image();
}

TEST(truncate_existing_via_fopen_ex_w) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int fd = loci_sdimg_fopen_ex(img, "T.BIN", 1);
    const char* a = "AAAA";
    loci_sdimg_fwrite(img, fd, a, 4);
    loci_sdimg_fclose(img, fd);

    /* Reopen for write → truncates. Write fewer bytes. */
    fd = loci_sdimg_fopen_ex(img, "T.BIN", 1);
    ASSERT_TRUE(fd >= 0);
    loci_sdimg_fwrite(img, fd, "B", 1);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);

    img = loci_sdimg_open(g_path);
    fd = loci_sdimg_fopen(img, "T.BIN");
    char buf[8] = {0};
    int br = loci_sdimg_fread(img, fd, buf, sizeof(buf));
    ASSERT_EQ(br, 1);
    ASSERT_EQ(buf[0], 'B');
    loci_sdimg_close(img);
    destroy_image();
}

TEST(unlink_removes_file_and_frees_clusters) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int fd = loci_sdimg_fopen_ex(img, "BYE.TXT", 1);
    uint8_t buf[200];
    memset(buf, 'X', sizeof(buf));
    loci_sdimg_fwrite(img, fd, buf, sizeof(buf));
    loci_sdimg_fclose(img, fd);

    int r = loci_sdimg_unlink(img, "BYE.TXT");
    ASSERT_EQ(r, 0);

    /* Should be reopen-able as a new file (entry freed). */
    fd = loci_sdimg_fopen_ex(img, "BYE.TXT", 1);
    ASSERT_TRUE(fd >= 0);
    loci_sdimg_fclose(img, fd);

    /* Re-unlink works, and re-unlink twice returns ENOENT. */
    ASSERT_EQ(loci_sdimg_unlink(img, "BYE.TXT"), 0);
    ASSERT_EQ(loci_sdimg_unlink(img, "BYE.TXT"), -ENOENT);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(rename_in_same_dir) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int fd = loci_sdimg_fopen_ex(img, "OLD.TXT", 1);
    loci_sdimg_fwrite(img, fd, "DATA", 4);
    loci_sdimg_fclose(img, fd);

    int r = loci_sdimg_rename(img, "OLD.TXT", "NEW.TXT");
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(loci_sdimg_fopen(img, "OLD.TXT") == -ENOENT);
    fd = loci_sdimg_fopen(img, "NEW.TXT");
    ASSERT_TRUE(fd >= 0);
    char buf[8] = {0};
    int br = loci_sdimg_fread(img, fd, buf, sizeof(buf));
    ASSERT_EQ(br, 4);
    ASSERT_TRUE(memcmp(buf, "DATA", 4) == 0);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(mkdir_creates_subdir_with_dot_entries) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int r = loci_sdimg_mkdir(img, "SUB");
    ASSERT_EQ(r, 0);

    /* Should be listable as a dir in root. */
    int dh = loci_sdimg_opendir(img, "");
    char name[64]; uint8_t attrib; uint32_t size;
    int found = 0;
    while (loci_sdimg_readdir(img, dh, name, &attrib, &size) == 1) {
        if (strcmp(name, "SUB") == 0) { found = 1; ASSERT_TRUE(attrib & 0x10); }
    }
    loci_sdimg_closedir(img, dh);
    ASSERT_TRUE(found);

    /* And we can opendir into it (it should appear empty after . and ..). */
    dh = loci_sdimg_opendir(img, "SUB");
    ASSERT_TRUE(dh >= 0);
    /* readdir skips "." and ".." → first call returns end-of-dir. */
    int rd = loci_sdimg_readdir(img, dh, name, &attrib, &size);
    ASSERT_EQ(rd, 0);
    loci_sdimg_closedir(img, dh);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(create_file_in_subdir) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    loci_sdimg_mkdir(img, "DOCS");
    int fd = loci_sdimg_fopen_ex(img, "DOCS/HI.TXT", 1);
    ASSERT_TRUE(fd >= 0);
    loci_sdimg_fwrite(img, fd, "in-sub", 6);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);

    img = loci_sdimg_open(g_path);
    fd = loci_sdimg_fopen(img, "DOCS/HI.TXT");
    ASSERT_TRUE(fd >= 0);
    char buf[8] = {0};
    ASSERT_EQ(loci_sdimg_fread(img, fd, buf, sizeof(buf)), 6);
    ASSERT_TRUE(memcmp(buf, "in-sub", 6) == 0);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(write_then_seek_overwrite) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    int fd = loci_sdimg_fopen_ex(img, "OW.BIN", 1);
    const char* a = "AAAAAAAAAA";
    loci_sdimg_fwrite(img, fd, a, 10);
    loci_sdimg_lseek(img, fd, 3, 0);
    loci_sdimg_fwrite(img, fd, "ZZ", 2);
    loci_sdimg_fclose(img, fd);
    loci_sdimg_close(img);

    img = loci_sdimg_open(g_path);
    fd = loci_sdimg_fopen(img, "OW.BIN");
    char buf[16] = {0};
    int br = loci_sdimg_fread(img, fd, buf, sizeof(buf));
    ASSERT_EQ(br, 10);
    ASSERT_TRUE(memcmp(buf, "AAAZZAAAAA", 10) == 0);
    loci_sdimg_close(img);
    destroy_image();
}

TEST(read_only_image_rejects_writes) {
    create_blank_image();
    /* Make the image read-only on the host. */
    chmod(g_path, 0444);
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    ASSERT_TRUE(img != NULL);
    int fd = loci_sdimg_fopen_ex(img, "X.TXT", 1);
    ASSERT_EQ(fd, -EROFS);
    ASSERT_EQ(loci_sdimg_unlink(img, "WHATEVER"), -EROFS);
    ASSERT_EQ(loci_sdimg_mkdir(img, "D"), -EROFS);
    loci_sdimg_close(img);
    chmod(g_path, 0644);
    destroy_image();
}

TEST(invalid_83_name_rejected) {
    create_blank_image();
    loci_sdimg_t* img = loci_sdimg_open(g_path);
    /* Name too long → reject */
    int fd = loci_sdimg_fopen_ex(img, "TOOLONGNAME.TXT", 1);
    ASSERT_EQ(fd, -EINVAL);
    /* Forbidden char → reject */
    fd = loci_sdimg_fopen_ex(img, "B*D.TXT", 1);
    ASSERT_TRUE(fd < 0);
    loci_sdimg_close(img);
    destroy_image();
}

int main(void) {
    printf("\n=== LOCI SDIMG write-path tests (Sprint 34ap) ===\n\n");

    RUN(open_blank_image_is_writable);
    RUN(create_small_file_round_trip);
    RUN(file_listed_in_root_after_create);
    RUN(write_across_cluster_boundary);
    RUN(truncate_existing_via_fopen_ex_w);
    RUN(unlink_removes_file_and_frees_clusters);
    RUN(rename_in_same_dir);
    RUN(mkdir_creates_subdir_with_dot_entries);
    RUN(create_file_in_subdir);
    RUN(write_then_seek_overwrite);
    RUN(read_only_image_rejects_writes);
    RUN(invalid_83_name_rejected);

    printf("\n===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("===========================================================\n");
    return tests_failed == 0 ? 0 : 1;
}
