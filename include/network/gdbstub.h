/**
 * @file gdbstub.h
 * @brief GDB Remote Serial Protocol (RSP) stub — debug the 6502 from GDB/IDEs
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * Exposes the emulated 6502 over a TCP socket speaking the GDB Remote Serial
 * Protocol, so `gdb`, lldb, or an IDE (VS Code, CLion) can attach with
 * `target remote :PORT` and set breakpoints, single-step, and inspect/modify
 * registers and memory. Reuses the existing debugger_t breakpoint/watchpoint
 * machinery, so GDB breakpoints and the native REPL share the same model.
 *
 * No external dependency (POSIX sockets only). Protocol logic (checksum,
 * packet framing, command dispatch) is split from the socket transport so it
 * can be unit-tested against an in-memory emulator.
 */

#ifndef NETWORK_GDBSTUB_H
#define NETWORK_GDBSTUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct emulator_s;   /* forward decl — gdbstub.c includes emulator.h */

#define GDB_DEFAULT_PORT 1234

/** @brief Action requested by a processed packet. */
typedef enum {
    GDB_ACT_NONE,      /**< stay stopped, keep reading packets */
    GDB_ACT_CONTINUE,  /**< resume free execution */
    GDB_ACT_STEP,      /**< single-step one instruction */
    GDB_ACT_DETACH     /**< detach (or kill) — stop serving */
} gdb_action_t;

/** @brief GDB stub state. */
typedef struct {
    int      listen_fd;     /**< listening socket (-1 if none) */
    int      conn_fd;       /**< active GDB connection (-1 if none) */
    uint16_t port;          /**< TCP port */
    bool     attached;      /**< a GDB client is connected */
    bool     resumed;       /**< last action resumed; report a stop on re-entry */
    bool     no_ack_mode;   /**< QStartNoAckMode negotiated */
    int      stop_signal;   /**< signal number to report (5=TRAP, 2=INT) */
} gdb_stub_t;

/**
 * @brief Open the listening socket and block until a GDB client connects.
 * @return true on success (client attached)
 */
bool gdb_stub_init(gdb_stub_t* stub, uint16_t port);

/** @brief Close connection and listening socket. */
void gdb_stub_close(gdb_stub_t* stub);

/**
 * @brief Stopped loop: serve packets until GDB resumes (continue/step) or
 *        detaches. Sends a stop reply first if the previous action resumed.
 *        Mutates the emulator (registers/memory/breakpoints) per GDB commands.
 */
void gdb_stub_stopped(gdb_stub_t* stub, struct emulator_s* emu);

/**
 * @brief During free execution, poll for a GDB interrupt (Ctrl-C, 0x03) or a
 *        client disconnect. Returns true if the target should stop now.
 */
bool gdb_stub_poll_interrupt(gdb_stub_t* stub);

/* ─── Pure protocol helpers (unit-tested, no socket) ─────────────────── */

/** @brief RSP checksum: sum of payload bytes modulo 256. */
uint8_t gdb_checksum(const char* data, size_t len);

/**
 * @brief Handle one packet payload (without `$`…`#cs` framing), writing the
 *        response payload (also unframed) into `resp`. Returns the action.
 */
gdb_action_t gdb_dispatch(gdb_stub_t* stub, struct emulator_s* emu,
                          const char* pkt, char* resp, size_t resp_size);

#endif /* NETWORK_GDBSTUB_H */
