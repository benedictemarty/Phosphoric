/**
 * @file control_queue.h
 * @brief Thread-safe command queue bridging producer threads to the
 *        single-threaded emulator loop (sprint 93, API REST Epic 2).
 *
 * The emulator runs its CPU/VIA/video in one thread, one frame at a time.
 * A control command that mutates state (reset, load-disk, savestate…) must
 * therefore execute on that thread, at a frame boundary — never from an
 * arbitrary producer thread (e.g. the future HTTP API worker) mid-instruction.
 *
 * This queue is the hand-off point. Producers call control_queue_submit(),
 * which enqueues a command line and blocks until the emulator loop has run it.
 * The loop calls control_queue_drain() once per frame; each command is executed
 * through control_dispatch() into a buffer sink and its reply handed back to the
 * waiting producer. Exactly one consumer (the emulator loop) is expected;
 * producers may be many.
 */

#ifndef CONTROL_QUEUE_H
#define CONTROL_QUEUE_H

#include <stddef.h>
#include "control.h"   /* control_result_t */

typedef struct emulator_s emulator_t;
typedef struct control_queue_s control_queue_t;

/** Create an empty queue. Returns NULL on allocation failure. */
control_queue_t* control_queue_create(void);

/**
 * @brief Shut the queue down and free it.
 *
 * Any producers still blocked in submit() are released with an empty reply and
 * a CONTROL_CONTINUE result. Safe to call with NULL.
 */
void control_queue_destroy(control_queue_t* q);

/**
 * @brief Producer side: submit @p line and block until the emulator loop runs it.
 *
 * On return the command has been executed on the emulator thread. If
 * @p reply_out is non-NULL it receives ownership of a NUL-terminated response
 * buffer (free with free()); @p reply_len, if non-NULL, receives its byte
 * length. If the queue is shutting down the call returns CONTROL_CONTINUE with
 * an empty reply. Thread-safe; may be called concurrently from many threads.
 */
control_result_t control_queue_submit(control_queue_t* q, const char* line,
                                      char** reply_out, size_t* reply_len);

/**
 * @brief Consumer side: execute every command queued at entry into @p emu.
 *
 * Intended to be called once per frame from the emulator main loop. Processes
 * the batch present when it starts (commands enqueued during the drain wait for
 * the next call, bounding per-frame work). Returns the number processed. A NULL
 * queue is a no-op returning 0, so the loop can call it unconditionally.
 */
int control_queue_drain(control_queue_t* q, emulator_t* emu);

#endif /* CONTROL_QUEUE_H */
