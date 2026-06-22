/**
 * @file movie.c
 * @brief Deterministic input record/replay (see movie.h)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 */

#include "utils/movie.h"
#include "utils/logging.h"

#include <stdlib.h>
#include <string.h>

#define MOVIE_MAGIC "PHOSPHORIC-MOVIE 1"

bool movie_record_open(movie_t* m, const char* path, uint8_t model) {
    memset(m, 0, sizeof(*m));
    m->fp = fopen(path, "w");
    if (!m->fp) {
        log_error("movie: cannot open '%s' for recording", path);
        return false;
    }
    m->mode = MOVIE_RECORD;
    m->model = model;
    m->have_last = false;
    fprintf(m->fp, "%s\n", MOVIE_MAGIC);
    fprintf(m->fp, "model %u\n", (unsigned)model);
    return true;
}

void movie_record_frame(movie_t* m, uint32_t frame, const uint8_t matrix[8]) {
    if (m->mode != MOVIE_RECORD || !m->fp) return;
    if (m->have_last && memcmp(matrix, m->last_matrix, 8) == 0) return; /* unchanged */
    fprintf(m->fp, "F %u", (unsigned)frame);
    for (int i = 0; i < 8; i++) fprintf(m->fp, " %02x", matrix[i]);
    fputc('\n', m->fp);
    memcpy(m->last_matrix, matrix, 8);
    m->have_last = true;
    m->event_count++;
}

bool movie_replay_open(movie_t* m, const char* path, uint8_t* model_out) {
    memset(m, 0, sizeof(*m));
    FILE* fp = fopen(path, "r");
    if (!fp) {
        log_error("movie: cannot open '%s' for replay", path);
        return false;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp) ||
        strncmp(line, MOVIE_MAGIC, strlen(MOVIE_MAGIC)) != 0) {
        log_error("movie: '%s' is not a Phosphoric movie", path);
        fclose(fp);
        return false;
    }

    m->mode = MOVIE_REPLAY;
    memset(m->cur_matrix, 0xFF, 8);   /* all keys released initially */

    uint32_t cap = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned mv;
        if (sscanf(line, "model %u", &mv) == 1) {
            m->model = (uint8_t)mv;
            continue;
        }
        unsigned fr, b[8];
        if (sscanf(line, "F %u %x %x %x %x %x %x %x %x", &fr,
                   &b[0], &b[1], &b[2], &b[3],
                   &b[4], &b[5], &b[6], &b[7]) == 9) {
            if (m->num_events >= cap) {
                cap = cap ? cap * 2 : 256;
                movie_event_t* ne =
                    (movie_event_t*)realloc(m->events, cap * sizeof(*ne));
                if (!ne) { free(m->events); fclose(fp); return false; }
                m->events = ne;
            }
            m->events[m->num_events].frame = fr;
            for (int i = 0; i < 8; i++)
                m->events[m->num_events].matrix[i] = (uint8_t)b[i];
            m->end_frame = fr;
            m->num_events++;
        }
    }
    fclose(fp);

    if (model_out) *model_out = m->model;
    log_info("movie: loaded %u input event(s) from '%s' (last frame %u)",
             m->num_events, path, m->end_frame);
    return true;
}

void movie_replay_frame(movie_t* m, uint32_t frame, uint8_t matrix[8]) {
    if (m->mode != MOVIE_REPLAY) return;
    /* Apply every event whose frame has been reached; the last one wins. */
    while (m->replay_idx < m->num_events &&
           m->events[m->replay_idx].frame <= frame) {
        memcpy(m->cur_matrix, m->events[m->replay_idx].matrix, 8);
        m->replay_idx++;
    }
    memcpy(matrix, m->cur_matrix, 8);
}

bool movie_replay_done(const movie_t* m) {
    return m->mode == MOVIE_REPLAY && m->replay_idx >= m->num_events;
}

void movie_close(movie_t* m) {
    if (m->fp) {
        fflush(m->fp);
        fclose(m->fp);
        m->fp = NULL;
        log_info("movie: recorded %u input event(s)", m->event_count);
    }
    free(m->events);
    m->events = NULL;
    m->num_events = 0;
    m->mode = MOVIE_OFF;
}
