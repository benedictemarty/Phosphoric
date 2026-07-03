/**
 * @file gdbstub_win.c
 * @brief GDB remote stub — Windows (MinGW-w64) placeholder, Sprint 89
 * @author bmarty <bmarty@mailo.com>
 *
 * The GDB stub uses POSIX sockets; the Windows v1 build links this
 * placeholder instead so --gdb fails with a clear message rather than
 * a build error. Porting to Winsock is tracked as follow-up work.
 */

#include "network/gdbstub.h"
#include "utils/logging.h"

bool gdb_stub_init(gdb_stub_t* stub, uint16_t port)
{
    (void)stub; (void)port;
    log_error("GDB stub: non disponible dans le build Windows v1 "
              "(sockets POSIX) — utilisez la version Linux/WSL2");
    return false;
}

void gdb_stub_close(gdb_stub_t* stub)
{
    (void)stub;
}

void gdb_stub_stopped(gdb_stub_t* stub, struct emulator_s* emu)
{
    (void)stub; (void)emu;
}

bool gdb_stub_poll_interrupt(gdb_stub_t* stub)
{
    (void)stub;
    return false;
}
