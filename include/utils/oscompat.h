/**
 * @file oscompat.h
 * @brief Windows (MinGW-w64) compatibility layer — Sprint 89
 * @author bmarty <bmarty@mailo.com>
 *
 * Single include that papers over the POSIX surface Phosphoric uses.
 * On POSIX platforms it includes the usual headers and defines no-ops;
 * on Windows it maps sockets to Winsock2 and shims the few libc gaps.
 *
 * Windows v1 scope (excluded, fail with a clear message at runtime):
 * pty and com serial transports (termios), --control async-pause
 * (select on stdin), CAST=1, MIDI=1, TLS picowifi (HAS_PICOTLS).
 */

#ifndef OSCOMPAT_H
#define OSCOMPAT_H

#ifdef _WIN32
/* ── Windows ─────────────────────────────────────────────────────── */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <time.h>

/* BSD socket surface. Winsock descriptors are SOCKET (unsigned), but the
 * values fit in int for the counts we use; the code treats -1 as invalid
 * which matches INVALID_SOCKET after the int cast. */
#define oscompat_close_socket(fd)  closesocket(fd)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0            /* no SIGPIPE on Windows anyway */
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

/* One-time Winsock startup (idempotent, called by socket users). */
static inline void oscompat_net_init(void) {
    static int done = 0;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = 1;
    }
}

/* mkdir(path, mode) → _mkdir(path) */
#define oscompat_mkdir(path, mode) _mkdir(path)

/* SIGPIPE does not exist on Windows */
#define oscompat_ignore_sigpipe() ((void)0)

/* statvfs → GetDiskFreeSpaceEx (only f_blocks * f_frsize is consumed) */
struct oscompat_statvfs { unsigned long long f_blocks, f_frsize; };
static inline int oscompat_statvfs(const char* path, struct oscompat_statvfs* out) {
    ULARGE_INTEGER total;
    if (!GetDiskFreeSpaceExA(path, NULL, &total, NULL)) return -1;
    out->f_frsize = 1;
    out->f_blocks = (unsigned long long)total.QuadPart;
    return 0;
}

/* NOTE: --realtime keeps using clock_gettime/clock_nanosleep directly —
 * MinGW-w64 provides both via winpthreads. */

#else
/* ── POSIX ───────────────────────────────────────────────────────── */

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#define oscompat_close_socket(fd)  close(fd)
static inline void oscompat_net_init(void) {}
#define oscompat_mkdir(path, mode) mkdir((path), (mode))
#define oscompat_ignore_sigpipe()  signal(SIGPIPE, SIG_IGN)

#define oscompat_statvfs statvfs_wrap
struct oscompat_statvfs { unsigned long long f_blocks, f_frsize; };
static inline int statvfs_wrap(const char* path, struct oscompat_statvfs* out) {
    struct statvfs vs;
    if (statvfs(path, &vs) != 0) return -1;
    out->f_blocks = vs.f_blocks;
    out->f_frsize = vs.f_frsize;
    return 0;
}

#endif /* _WIN32 */

#endif /* OSCOMPAT_H */
