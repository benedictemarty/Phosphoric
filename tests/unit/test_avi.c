/**
 * @file test_avi.c
 * @brief Motion-JPEG AVI recorder tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * Validates the MJPEG AVI writer (src/video/avi_recorder.c):
 *   - file is created and well-formed (RIFF/AVI /hdrl/movi/idx1 fourccs)
 *   - back-patched sizes are consistent (RIFF size, movi size, frame counts)
 *   - the idx1 index has one keyframe entry per frame with valid offsets
 *   - empty recording and the no-op close path behave
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "video/avi_recorder.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-55s", #name); \
    int _before = tests_failed; \
    name(); \
    if (tests_failed == _before) { \
        tests_passed++; \
        printf("PASS\n"); \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((long)(a) != (long)(b)) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", __FILE__, __LINE__, \
               (long)(b), (long)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define TMP_AVI "/tmp/phosphoric_test.avi"

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int find_fourcc(const uint8_t* buf, long len, const char* cc) {
    for (long i = 0; i + 4 <= len; i++) {
        if (memcmp(buf + i, cc, 4) == 0) return (int)i;
    }
    return -1;
}

/* Fill an RGB888 buffer with a frame-dependent gradient. */
static void make_frame(uint8_t* rgb, int w, int h, int seed) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t* p = rgb + (y * w + x) * 3;
            p[0] = (uint8_t)(x * 4 + seed);
            p[1] = (uint8_t)(y * 4 + seed);
            p[2] = (uint8_t)(seed * 8);
        }
    }
}

/* Read the whole file into a malloc'd buffer; returns length (-1 on error). */
static long slurp(const char* path, uint8_t** out) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)n);
    if (!b) { fclose(fp); return -1; }
    size_t got = fread(b, 1, (size_t)n, fp);
    fclose(fp);
    if ((long)got != n) { free(b); return -1; }
    *out = b;
    return n;
}

/* ═══════════════════════════════════════════════════════════════ */

TEST(test_avi_open_close_basic) {
    avi_recorder_t rec;
    ASSERT_TRUE(avi_recorder_open(&rec, TMP_AVI, 32, 24, 50, 80));

    const int W = 32, H = 24, N = 10;
    uint8_t* frame = (uint8_t*)malloc((size_t)(W * H * 3));
    ASSERT_TRUE(frame != NULL);
    for (int i = 0; i < N; i++) {
        make_frame(frame, W, H, i);
        ASSERT_TRUE(avi_recorder_add_frame(&rec, frame));
    }
    free(frame);
    ASSERT_EQ(rec.frame_count, N);
    ASSERT_TRUE(avi_recorder_close(&rec));
    /* close() resets the struct */
    ASSERT_EQ(rec.frame_count, 0);
    ASSERT_TRUE(rec.fp == NULL);
}

TEST(test_avi_structure_fourccs) {
    avi_recorder_t rec;
    const int W = 16, H = 16, N = 5;
    ASSERT_TRUE(avi_recorder_open(&rec, TMP_AVI, W, H, 25, 75));
    uint8_t* frame = (uint8_t*)malloc((size_t)(W * H * 3));
    for (int i = 0; i < N; i++) { make_frame(frame, W, H, i * 7);
                                  avi_recorder_add_frame(&rec, frame); }
    free(frame);
    avi_recorder_close(&rec);

    uint8_t* buf = NULL;
    long len = slurp(TMP_AVI, &buf);
    ASSERT_TRUE(len > 0);

    /* Mandatory fourccs present and in the right relative order. */
    ASSERT_EQ(memcmp(buf, "RIFF", 4), 0);
    ASSERT_EQ(memcmp(buf + 8, "AVI ", 4), 0);
    int hdrl = find_fourcc(buf, len, "hdrl");
    int avih = find_fourcc(buf, len, "avih");
    int strh = find_fourcc(buf, len, "strh");
    int mjpg = find_fourcc(buf, len, "MJPG");
    int movi = find_fourcc(buf, len, "movi");
    int idx1 = find_fourcc(buf, len, "idx1");
    ASSERT_TRUE(hdrl > 0 && avih > hdrl && strh > avih);
    ASSERT_TRUE(mjpg > 0 && movi > strh && idx1 > movi);

    /* RIFF size field == file length - 8. */
    ASSERT_EQ(rd_u32(buf + 4), (uint32_t)(len - 8));

    /* idx1 size field == N * 16, and one '00dc' keyframe entry per frame. */
    ASSERT_EQ(rd_u32(buf + idx1 + 4), (uint32_t)(N * 16));
    for (int i = 0; i < N; i++) {
        const uint8_t* e = buf + idx1 + 8 + i * 16;
        ASSERT_EQ(memcmp(e, "00dc", 4), 0);
        ASSERT_EQ(rd_u32(e + 4), 0x10u);  /* AVIIF_KEYFRAME */
    }
    free(buf);
}

TEST(test_avi_frame_counts_patched) {
    avi_recorder_t rec;
    const int W = 16, H = 16, N = 7;
    ASSERT_TRUE(avi_recorder_open(&rec, TMP_AVI, W, H, 50, 80));
    uint8_t* frame = (uint8_t*)malloc((size_t)(W * H * 3));
    for (int i = 0; i < N; i++) { make_frame(frame, W, H, i);
                                  avi_recorder_add_frame(&rec, frame); }
    free(frame);
    avi_recorder_close(&rec);

    uint8_t* buf = NULL;
    long len = slurp(TMP_AVI, &buf);
    ASSERT_TRUE(len > 0);

    int avih = find_fourcc(buf, len, "avih");
    int strh = find_fourcc(buf, len, "strh");
    /* avih dwTotalFrames is at chunk-data offset +16 (after 8-byte header). */
    ASSERT_EQ(rd_u32(buf + avih + 8 + 16), (uint32_t)N);
    /* strh dwLength is at chunk-data offset +32. */
    ASSERT_EQ(rd_u32(buf + strh + 8 + 32), (uint32_t)N);
    free(buf);
}

TEST(test_avi_idx_offsets_point_to_chunks) {
    avi_recorder_t rec;
    const int W = 16, H = 16, N = 4;
    ASSERT_TRUE(avi_recorder_open(&rec, TMP_AVI, W, H, 50, 80));
    uint8_t* frame = (uint8_t*)malloc((size_t)(W * H * 3));
    for (int i = 0; i < N; i++) { make_frame(frame, W, H, i * 11);
                                  avi_recorder_add_frame(&rec, frame); }
    free(frame);
    avi_recorder_close(&rec);

    uint8_t* buf = NULL;
    long len = slurp(TMP_AVI, &buf);
    ASSERT_TRUE(len > 0);

    int movi = find_fourcc(buf, len, "movi");
    int idx1 = find_fourcc(buf, len, "idx1");
    ASSERT_TRUE(movi > 0 && idx1 > movi);

    /* Each idx1 offset is relative to the 'movi' fourcc and must land on the
     * matching '00dc' chunk header, whose size field equals the index size. */
    for (int i = 0; i < N; i++) {
        const uint8_t* e = buf + idx1 + 8 + i * 16;
        uint32_t off = rd_u32(e + 8);
        uint32_t sz  = rd_u32(e + 12);
        long chunk = movi + (long)off;
        ASSERT_TRUE(chunk + 8 <= len);
        ASSERT_EQ(memcmp(buf + chunk, "00dc", 4), 0);
        ASSERT_EQ(rd_u32(buf + chunk + 4), sz);
    }
    /* First chunk sits right after the 'movi' fourcc → offset 4. */
    ASSERT_EQ(rd_u32(buf + idx1 + 8 + 8), 4u);
    free(buf);
}

TEST(test_avi_empty_recording) {
    avi_recorder_t rec;
    ASSERT_TRUE(avi_recorder_open(&rec, TMP_AVI, 16, 16, 50, 80));
    ASSERT_TRUE(avi_recorder_close(&rec));

    uint8_t* buf = NULL;
    long len = slurp(TMP_AVI, &buf);
    ASSERT_TRUE(len > 0);
    ASSERT_EQ(memcmp(buf, "RIFF", 4), 0);
    ASSERT_EQ(rd_u32(buf + 4), (uint32_t)(len - 8));
    int avih = find_fourcc(buf, len, "avih");
    ASSERT_EQ(rd_u32(buf + avih + 8 + 16), 0u);  /* zero frames */
    free(buf);
}

TEST(test_avi_close_noop_safe) {
    /* close() on a zeroed/unopened recorder must be a safe no-op. */
    avi_recorder_t rec;
    memset(&rec, 0, sizeof(rec));
    ASSERT_TRUE(avi_recorder_close(&rec));
}

TEST(test_avi_open_invalid_args) {
    avi_recorder_t rec;
    ASSERT_TRUE(!avi_recorder_open(&rec, TMP_AVI, 0, 16, 50, 80));
    ASSERT_TRUE(!avi_recorder_open(&rec, TMP_AVI, 16, 16, 0, 80));
    ASSERT_TRUE(!avi_recorder_open(&rec, NULL, 16, 16, 50, 80));
}

TEST(test_avi_audio_stream) {
    /* open_av with audio declares a second 'auds' PCM stream and interleaves
     * '01wb' chunks with the video frames. */
    avi_recorder_t rec;
    const int W = 16, H = 16, RATE = 44100, CH = 2, NF = 100, FRAMES = 3;
    ASSERT_TRUE(avi_recorder_open_av(&rec, TMP_AVI, W, H, 50, 80, RATE, CH));
    ASSERT_TRUE(rec.has_audio);

    uint8_t* frame = (uint8_t*)malloc((size_t)(W * H * 3));
    ASSERT_TRUE(frame != NULL);
    int16_t samples[100 * 2];
    for (int i = 0; i < NF * 2; i++) samples[i] = (int16_t)(i * 7 - 1000);

    for (int f = 0; f < FRAMES; f++) {
        make_frame(frame, W, H, f);
        ASSERT_TRUE(avi_recorder_add_frame(&rec, frame));
        ASSERT_TRUE(avi_recorder_add_audio(&rec, samples, NF));
    }
    free(frame);
    ASSERT_EQ(rec.audio_frames, (uint32_t)(FRAMES * NF));
    ASSERT_TRUE(avi_recorder_close(&rec));

    uint8_t* buf = NULL;
    long len = slurp(TMP_AVI, &buf);
    ASSERT_TRUE(len > 0);

    /* avih dwStreams = 2 (data+24). */
    int avih = find_fourcc(buf, len, "avih");
    ASSERT_TRUE(avih > 0);
    ASSERT_EQ(rd_u32(buf + avih + 8 + 24), 2u);

    /* Audio stream header present, dwLength patched = total sample-frames.
     * 'auds' is the strh fccType (strh data start); dwLength is at +32. */
    int auds = find_fourcc(buf, len, "auds");
    ASSERT_TRUE(auds > 0);
    ASSERT_EQ(rd_u32(buf + auds + 32), (uint32_t)(FRAMES * NF));

    /* Interleaved audio chunks + idx1 has both video and audio entries. */
    ASSERT_TRUE(find_fourcc(buf, len, "01wb") > 0);
    int idx1 = find_fourcc(buf, len, "idx1");
    ASSERT_TRUE(idx1 > 0);
    ASSERT_EQ(rd_u32(buf + idx1 + 4), (uint32_t)((FRAMES * 2) * 16));
    free(buf);
}

TEST(test_avi_no_audio_is_video_only) {
    /* open_av with rate 0 is byte-equivalent to plain open : one stream, no 'auds'. */
    avi_recorder_t rec;
    ASSERT_TRUE(avi_recorder_open_av(&rec, TMP_AVI, 16, 16, 50, 80, 0, 0));
    ASSERT_TRUE(!rec.has_audio);
    ASSERT_TRUE(avi_recorder_close(&rec));

    uint8_t* buf = NULL;
    long len = slurp(TMP_AVI, &buf);
    ASSERT_TRUE(len > 0);
    int avih = find_fourcc(buf, len, "avih");
    ASSERT_EQ(rd_u32(buf + avih + 8 + 24), 1u);      /* dwStreams = 1 */
    ASSERT_EQ(find_fourcc(buf, len, "auds"), -1);     /* no audio stream */
    free(buf);
}

int main(void) {
    printf("Running AVI recorder tests...\n\n");

    RUN(test_avi_open_close_basic);
    RUN(test_avi_structure_fourccs);
    RUN(test_avi_frame_counts_patched);
    RUN(test_avi_idx_offsets_point_to_chunks);
    RUN(test_avi_empty_recording);
    RUN(test_avi_close_noop_safe);
    RUN(test_avi_open_invalid_args);
    RUN(test_avi_audio_stream);
    RUN(test_avi_no_audio_is_video_only);

    remove(TMP_AVI);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
