/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_common.h
 * @brief Helpers shared by the fuzzing targets (tests/fuzz/)
 * @author bmarty <bmarty@mailo.com>
 *
 * Each target defines LLVMFuzzerTestOneInput(): with clang
 * (-fsanitize=fuzzer, `make fuzz`) libFuzzer provides main(); with gcc,
 * fuzz_replay.c replays a corpus (`make test-fuzz-replay`). A target may
 * also define fuzz_make_seed() to build a valid seed.
 *
 * Phosphoric's parsers take paths: the input is written to a temporary file,
 * reused from one call to the next.
 */
#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/* Optional seed (fuzz_replay --seed PATH): 0 = written. */
int fuzz_make_seed(const char* path) __attribute__((weak));

static char fuzz_tmp_path[64];
static void fuzz_tmp_remove(void) { if (fuzz_tmp_path[0]) unlink(fuzz_tmp_path); }

static inline const char* fuzz_tmpfile(const uint8_t* data, size_t size) {
    char* path = fuzz_tmp_path;
    if (!path[0]) {
        snprintf(path, sizeof fuzz_tmp_path, "/tmp/phosphoric_fuzz_%ld", (long)getpid());
        atexit(fuzz_tmp_remove);
    }
    FILE* f = fopen(path, "wb");
    if (!f) abort();
    if (size && fwrite(data, 1, size, f) != size) abort();
    fclose(f);
    return path;
}

/* Silent log: an invalid input produces expected errors, and writing them
 * would slow fuzzing down. For the targets that link utils/logging.c. */
#define FUZZ_QUIET_LOGS() do {                              \
        static int fuzz_quiet_done;                         \
        if (!fuzz_quiet_done) {                             \
            fuzz_quiet_done = 1;                            \
            log_init(LOG_LEVEL_ERROR);                      \
            FILE* fuzz_null = fopen("/dev/null", "w");      \
            if (fuzz_null) log_set_stream(fuzz_null);       \
        }                                                   \
    } while (0)

#endif /* FUZZ_COMMON_H */
