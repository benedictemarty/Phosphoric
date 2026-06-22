/**
 * @file test_movie.c
 * @brief Deterministic input record/replay — movie format tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * Validates the movie module (src/utils/movie.c): record writes change-only
 * events, replay reproduces the exact per-frame matrix (holding state between
 * change records), the round-trip is bit-exact, model id survives, and
 * malformed input is rejected.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "utils/movie.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    int _before = tests_failed; \
    name(); \
    if (tests_failed == _before) { tests_passed++; printf("PASS\n"); } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
                tests_failed++; return; } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((long)(a) != (long)(b)) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", __FILE__, __LINE__, \
               (long)(b), (long)(a)); tests_failed++; return; } \
} while(0)

#define TMP "/tmp/phosphoric_test.phm"

static void mat(uint8_t m[8], uint8_t fill) { memset(m, fill, 8); }

/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_record_change_only) {
    movie_t m;
    ASSERT_TRUE(movie_record_open(&m, TMP, 1));
    uint8_t k[8];
    mat(k, 0xFF);
    movie_record_frame(&m, 0, k);     /* event 1 (first) */
    movie_record_frame(&m, 1, k);     /* unchanged → skip */
    movie_record_frame(&m, 2, k);     /* unchanged → skip */
    k[0] = 0xFE;
    movie_record_frame(&m, 3, k);     /* event 2 (changed) */
    movie_record_frame(&m, 4, k);     /* unchanged → skip */
    ASSERT_EQ(m.event_count, 2);
    movie_close(&m);
}

TEST(test_roundtrip_exact) {
    /* Record a small scripted sequence. */
    movie_t m;
    ASSERT_TRUE(movie_record_open(&m, TMP, 1));
    uint8_t k[8];
    mat(k, 0xFF);            movie_record_frame(&m, 0, k);
    k[2] = 0xF7;             movie_record_frame(&m, 5, k);   /* press */
    mat(k, 0xFF);            movie_record_frame(&m, 8, k);   /* release */
    movie_close(&m);

    /* Replay and check the matrix at representative frames. */
    movie_t r;
    uint8_t model = 0;
    ASSERT_TRUE(movie_replay_open(&r, TMP, &model));
    ASSERT_EQ(model, 1);

    uint8_t out[8];
    /* frames 0-4: all released */
    for (uint32_t f = 0; f < 5; f++) {
        movie_replay_frame(&r, f, out);
        ASSERT_EQ(out[2], 0xFF);
    }
    /* frames 5-7: key held (col2 = 0xF7) */
    for (uint32_t f = 5; f < 8; f++) {
        movie_replay_frame(&r, f, out);
        ASSERT_EQ(out[2], 0xF7);
    }
    /* frame 8+: released again */
    movie_replay_frame(&r, 8, out);
    ASSERT_EQ(out[2], 0xFF);
    ASSERT_EQ(r.end_frame, 8);
    movie_close(&r);
}

TEST(test_replay_holds_state_with_gaps) {
    /* A change at frame 10, queried only at frame 100, must still apply. */
    movie_t m;
    movie_record_open(&m, TMP, 0);
    uint8_t k[8]; mat(k, 0xFF);
    movie_record_frame(&m, 0, k);
    k[5] = 0x7F; movie_record_frame(&m, 10, k);
    movie_close(&m);

    movie_t r;
    ASSERT_TRUE(movie_replay_open(&r, TMP, NULL));
    uint8_t out[8];
    movie_replay_frame(&r, 100, out);    /* jump past the change */
    ASSERT_EQ(out[5], 0x7F);
    movie_close(&r);
}

TEST(test_replay_done) {
    movie_t m;
    movie_record_open(&m, TMP, 0);
    uint8_t k[8]; mat(k, 0xFF); k[0] = 0x01;
    movie_record_frame(&m, 2, k);
    movie_close(&m);

    movie_t r;
    movie_replay_open(&r, TMP, NULL);
    uint8_t out[8];
    movie_replay_frame(&r, 0, out);
    ASSERT_TRUE(!movie_replay_done(&r));    /* event at frame 2 not yet applied */
    movie_replay_frame(&r, 2, out);
    ASSERT_TRUE(movie_replay_done(&r));     /* drained */
    movie_close(&r);
}

TEST(test_bad_file_rejected) {
    /* Not a movie file. */
    FILE* f = fopen(TMP, "w");
    fputs("garbage\n", f);
    fclose(f);
    movie_t r;
    ASSERT_TRUE(!movie_replay_open(&r, TMP, NULL));
    /* Missing file. */
    ASSERT_TRUE(!movie_replay_open(&r, "/tmp/does_not_exist_xyz.phm", NULL));
}

int main(void) {
    printf("\nMovie record/replay tests\n\n");
    RUN(test_record_change_only);
    RUN(test_roundtrip_exact);
    RUN(test_replay_holds_state_with_gaps);
    RUN(test_replay_done);
    RUN(test_bad_file_rejected);
    remove(TMP);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
