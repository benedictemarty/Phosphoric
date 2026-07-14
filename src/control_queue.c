/**
 * @file control_queue.c
 * @brief Thread-safe command queue (sprint 93, API REST Epic 2).
 *
 * See control_queue.h for the model. Design notes:
 *
 *  - One shared mutex guards the linked list. Each queued command carries its
 *    own condition variable so a producer waits only on its own completion.
 *  - The consumer pops an item under the lock, then releases the lock for the
 *    (potentially slow) control_dispatch() so other producers can keep
 *    enqueueing. It re-takes the lock only to publish `done` + signal.
 *  - A popped item is referenced solely by the consumer and its owning
 *    (blocked) producer, so it stays alive until the producer frees it after
 *    waking. The mutex release/acquire pair gives the producer a happens-before
 *    view of the reply the consumer wrote before signalling.
 */

#define _POSIX_C_SOURCE 200809L
#include "control_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct control_cmd_s {
    char*                 line;    /**< command text, owned by the producer   */
    control_sink_t        reply;   /**< buffer sink filled by the consumer    */
    control_result_t      result;  /**< dispatch result                       */
    bool                  done;    /**< set true once executed (under mutex)  */
    pthread_cond_t        cv;      /**< producer waits here for `done`         */
    struct control_cmd_s* next;
} control_cmd_t;

struct control_queue_s {
    control_cmd_t*  head;
    control_cmd_t*  tail;
    int             count;
    bool            shutting_down;
    pthread_mutex_t mutex;
};

control_queue_t* control_queue_create(void) {
    control_queue_t* q = (control_queue_t*)calloc(1, sizeof(*q));
    if (!q) return NULL;
    if (pthread_mutex_init(&q->mutex, NULL) != 0) { free(q); return NULL; }
    return q;
}

/* Detach and return the head item; caller must hold the mutex. */
static control_cmd_t* dequeue_locked(control_queue_t* q) {
    control_cmd_t* item = q->head;
    if (!item) return NULL;
    q->head = item->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    item->next = NULL;
    return item;
}

void control_queue_destroy(control_queue_t* q) {
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    q->shutting_down = true;
    /* Release anyone still blocked with an empty reply. */
    for (control_cmd_t* it = q->head; it; it = it->next) {
        it->result = CONTROL_CONTINUE;
        it->done = true;
        pthread_cond_signal(&it->cv);
    }
    /* Producers own their items and will free them on wake; drop our list
     * references so we don't touch freed memory. */
    q->head = q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    free(q);
}

control_result_t control_queue_submit(control_queue_t* q, const char* line,
                                      char** reply_out, size_t* reply_len) {
    if (reply_out) *reply_out = NULL;
    if (reply_len) *reply_len = 0;
    if (!q || !line) return CONTROL_CONTINUE;

    control_cmd_t* item = (control_cmd_t*)calloc(1, sizeof(*item));
    if (!item) return CONTROL_CONTINUE;
    item->line = strdup(line);
    control_sink_init_buffer(&item->reply);
    item->done = false;
    pthread_cond_init(&item->cv, NULL);

    pthread_mutex_lock(&q->mutex);
    if (q->shutting_down) {
        pthread_mutex_unlock(&q->mutex);
        pthread_cond_destroy(&item->cv);
        control_sink_free(&item->reply);
        free(item->line);
        free(item);
        return CONTROL_CONTINUE;
    }
    /* enqueue at tail */
    if (q->tail) q->tail->next = item; else q->head = item;
    q->tail = item;
    q->count++;
    /* Block until the consumer marks this item done. */
    while (!item->done)
        pthread_cond_wait(&item->cv, &q->mutex);
    pthread_mutex_unlock(&q->mutex);

    /* The item is now ours alone. */
    control_result_t r = item->result;
    if (reply_out) {
        *reply_out = item->reply.buf;   /* transfer ownership */
        if (reply_len) *reply_len = item->reply.len;
        item->reply.buf = NULL;
    } else {
        control_sink_free(&item->reply);
    }
    pthread_cond_destroy(&item->cv);
    free(item->line);
    free(item);
    return r;
}

int control_queue_drain(control_queue_t* q, emulator_t* emu) {
    if (!q) return 0;
    pthread_mutex_lock(&q->mutex);
    int batch = q->count;   /* snapshot: bound work to what is queued now */
    int processed = 0;
    while (processed < batch) {
        control_cmd_t* item = dequeue_locked(q);
        if (!item) break;
        pthread_mutex_unlock(&q->mutex);

        /* Execute on the emulator thread. control_dispatch mutates the line
         * in place (strtok_r); the producer never reads it again, so passing
         * item->line directly is safe. */
        item->result = control_dispatch(emu, &item->reply, item->line);

        pthread_mutex_lock(&q->mutex);
        item->done = true;
        pthread_cond_signal(&item->cv);
        processed++;
    }
    pthread_mutex_unlock(&q->mutex);
    return processed;
}
