/**
 * @file control.h
 * @brief IPC control mode for OricForge IDE integration (sprint 35a)
 *
 * Line-based protocol on stdin/stdout. Three message kinds :
 *   CMD  (client → emu)   ex: "step", "read $BB80 16"
 *   REP  (emu → client)   ex: "OK pc=0502 cycles=1234"
 *   EVT  (emu → client)   ex: "EVT stopped pc=0510 reason=break id=2"
 *
 * Activated via the --control CLI flag. Logs are routed to stderr by
 * the entry hook so stdout stays clean for protocol traffic.
 */

#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct emulator_s emulator_t;

/* ─── response sink (sprint 92, Epic 1) ────────────────────────────
 * Decouples command handlers from their output channel so the same
 * dispatch feeds both the stdin/stdout IPC protocol and (future) an
 * HTTP API. In `stream` mode writes go straight to `fp` and are
 * flushed, reproducing the historical byte-for-byte stdout behaviour.
 * In `buffer` mode output accumulates in a growable, binary-safe
 * buffer the caller can inspect (and must free with control_sink_free). */
typedef struct control_sink_s {
    FILE*  fp;     /**< non-NULL => write-through stream (stdout mode) */
    char*  buf;    /**< else => accumulation buffer (HTTP/buffer mode) */
    size_t len;    /**< bytes used in buf (excludes NUL terminator)    */
    size_t cap;    /**< allocated capacity of buf                      */
    bool   error;  /**< set on a failed write / allocation             */
} control_sink_t;

/** Outcome of dispatching one command line. */
typedef enum {
    CONTROL_CONTINUE = 0,  /**< keep reading commands                  */
    CONTROL_RESUME   = 1,  /**< resume CPU (step/next/step-out/continue)*/
    CONTROL_QUIT     = 2   /**< client asked to quit                   */
} control_result_t;

/** Initialise a sink that writes through to @p fp (e.g. stdout). */
void control_sink_init_stream(control_sink_t* s, FILE* fp);
/** Initialise a sink that accumulates output in memory. */
void control_sink_init_buffer(control_sink_t* s);
/** Release a buffer-mode sink's storage (no-op in stream mode). */
void control_sink_free(control_sink_t* s);

/**
 * @brief Parse and execute one command line into @p sink.
 *
 * @p line is tokenised in place (mutated). The reply (REP) is written to
 * @p sink; asynchronous events (EVT) are not produced here. Returns whether
 * the caller should keep reading, resume the CPU, or quit.
 */
control_result_t control_dispatch(emulator_t* emu, control_sink_t* sink,
                                  char* line);

/**
 * @brief Read and dispatch one batch of commands from stdin, blocking.
 *
 * Returns when the client issues `step`, `next` or `continue`. Synchronous
 * commands (read, write, regs, break, ...) emit REP inline; resume commands
 * emit REP OK then hand control back to the main loop, which executes one
 * or more CPU steps and re-enters control_repl on the next break, where an
 * `EVT stopped …` is emitted.
 */
void control_repl(emulator_t* emu);

/**
 * @brief Emit the initial banner once the emulator is wired up but before
 * the CPU starts running. Tells the client the IPC channel is ready.
 */
void control_emit_ready(emulator_t* emu);

/**
 * @brief Emit an `EVT stopped …` event describing why the emulator just
 * paused. Called by the main loop right before re-entering control_repl.
 */
void control_emit_stopped(emulator_t* emu, const char* reason);

/**
 * @brief Non-blocking stdin poll called once per frame while the CPU is
 * running. Returns true if the client requested `pause` (or `quit`) and
 * the main loop should hand control back to the REPL. Other commands
 * during running are rejected with `ERR busy` (no queueing).
 */
bool control_poll_pause(emulator_t* emu);

/**
 * @brief Emit `EVT halt reason=<reason>` when the main loop exits for a
 * terminal cause (cycle_limit, jam). After this the process is about to
 * exit, so no further protocol traffic is expected.
 */
void control_emit_halt(emulator_t* emu, const char* reason);

#endif /* CONTROL_H */
