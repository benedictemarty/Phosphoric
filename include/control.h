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

typedef struct emulator_s emulator_t;

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

#endif /* CONTROL_H */
