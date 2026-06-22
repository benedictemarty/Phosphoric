/**
 * @file movie.h
 * @brief Deterministic input record/replay ("TAS movie")
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * Records the only non-deterministic input to the emulation — the 8-byte
 * keyboard matrix — at frame granularity, and replays it bit-exactly. Given
 * the same ROM/model and deterministic RAM init, replaying a movie reproduces
 * a session exactly: useful for tool-assisted runs, bug repro, and CI
 * regression (record once → replay headless → screenshot/compare).
 *
 * The matrix is the actual hardware input state (active-low, 8 columns), so
 * recording is decoupled from SDL keycodes/layout. Only changes are stored;
 * replay holds the last state between change records.
 *
 * File format (text, diffable):
 *   PHOSPHORIC-MOVIE 1
 *   model <0|1>
 *   F <frame> <m0> <m1> ... <m7>     (frame index + 8 hex matrix bytes)
 */

#ifndef UTILS_MOVIE_H
#define UTILS_MOVIE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    MOVIE_OFF = 0,
    MOVIE_RECORD,
    MOVIE_REPLAY
} movie_mode_t;

typedef struct {
    uint32_t frame;
    uint8_t  matrix[8];
} movie_event_t;

typedef struct {
    movie_mode_t mode;
    uint8_t      model;        /**< machine model id recorded/expected */

    /* record */
    FILE*    fp;
    uint8_t  last_matrix[8];
    bool     have_last;
    uint32_t event_count;

    /* replay */
    movie_event_t* events;
    uint32_t       num_events;
    uint32_t       replay_idx;     /**< next event to apply */
    uint8_t        cur_matrix[8];  /**< current replayed matrix state */
    uint32_t       end_frame;      /**< frame of the last event */
} movie_t;

/**
 * @brief Open a movie for recording. Writes the header.
 * @return true on success
 */
bool movie_record_open(movie_t* m, const char* path, uint8_t model);

/**
 * @brief Open a movie for replay. Loads all events into memory.
 * @param model_out  if non-NULL, receives the model id stored in the movie
 * @return true on success
 */
bool movie_replay_open(movie_t* m, const char* path, uint8_t* model_out);

/**
 * @brief Record the matrix for `frame` (writes a line only when it changed).
 */
void movie_record_frame(movie_t* m, uint32_t frame, const uint8_t matrix[8]);

/**
 * @brief Overwrite `matrix` with the replayed state for `frame`.
 */
void movie_replay_frame(movie_t* m, uint32_t frame, uint8_t matrix[8]);

/** @brief True once all recorded events have been applied (replay drained). */
bool movie_replay_done(const movie_t* m);

/** @brief Close the movie, flush a recording, free replay buffers. */
void movie_close(movie_t* m);

#endif /* UTILS_MOVIE_H */
