/**
 * @file serial_backend.c
 * @brief Serial backend implementations: loopback, TCP, PTY
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-22
 *
 * Pluggable backends for the ACIA 6551 serial interface.
 */

/* Required for getaddrinfo, openpty with -std=c11 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>

/* PTY support (POSIX) */
#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#include <pty.h>
#define HAS_PTY 1
#else
#define HAS_PTY 0
#endif

#include <time.h>          /* clock_gettime — SMF real-time pacing */
#include "io/smf.h"

/* Real-time host MIDI support — optional (MIDI=1). Platform-specific transport:
 *   Linux   → ALSA sequencer (-lasound)
 *   macOS   → CoreMIDI (-framework CoreMIDI -framework CoreFoundation)
 *   Windows → WinMM (-lwinmm)
 * Only the host platform's branch is compiled; the others never touch the build. */
#ifdef HAS_MIDI
#  if defined(__linux__)
#    include <alsa/asoundlib.h>
#    define MIDI_BACKEND_ALSA 1
#  elif defined(__APPLE__)
#    include <CoreMIDI/CoreMIDI.h>
#    include <CoreFoundation/CoreFoundation.h>
#    define MIDI_BACKEND_COREMIDI 1
#  elif defined(_WIN32)
#    include <windows.h>
#    include <mmsystem.h>
#    define MIDI_BACKEND_WINMM 1
#  endif
#endif

/* COM (real serial port) support — Linux only */
#if defined(__linux__)
#include <termios.h>
#include <sys/ioctl.h>
#define HAS_COM 1
#else
#define HAS_COM 0
#endif

#include "io/serial_backend.h"
#include "utils/logging.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  EINTR-safe read/write helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static ssize_t safe_read(int fd, void* buf, size_t count)
{
    ssize_t n;
    do { n = read(fd, buf, count); } while (n < 0 && errno == EINTR);
    return n;
}

static ssize_t safe_write(int fd, const void* buf, size_t count)
{
    ssize_t n;
    do { n = write(fd, buf, count); } while (n < 0 && errno == EINTR);
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  LOOPBACK backend — TX → circular buffer → RX
 * ═══════════════════════════════════════════════════════════════════════ */

static bool loopback_open(serial_backend_t* self)
{
    self->state.loopback.head = 0;
    self->state.loopback.tail = 0;
    self->state.loopback.count = 0;
    log_info("Serial loopback backend opened");
    return true;
}

static void loopback_close(serial_backend_t* self)
{
    self->state.loopback.count = 0;
    log_info("Serial loopback backend closed");
}

static bool loopback_send(serial_backend_t* self, uint8_t byte)
{
    if (self->state.loopback.count >= SERIAL_LOOPBACK_BUFSZ) {
        return false;  /* Buffer full */
    }
    self->state.loopback.buf[self->state.loopback.head] = byte;
    self->state.loopback.head = (self->state.loopback.head + 1) % SERIAL_LOOPBACK_BUFSZ;
    self->state.loopback.count++;
    return true;
}

static bool loopback_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.loopback.count <= 0) {
        return false;  /* Buffer empty */
    }
    *byte = self->state.loopback.buf[self->state.loopback.tail];
    self->state.loopback.tail = (self->state.loopback.tail + 1) % SERIAL_LOOPBACK_BUFSZ;
    self->state.loopback.count--;
    return true;
}

static bool loopback_poll(serial_backend_t* self)
{
    return self->state.loopback.count > 0;
}

static bool loopback_connected(serial_backend_t* self)
{
    (void)self;
    return true;
}

serial_backend_t* serial_backend_loopback_create(void)
{
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    b->type = SERIAL_BACKEND_LOOPBACK;
    b->open = loopback_open;
    b->close = loopback_close;
    b->send = loopback_send;
    b->recv = loopback_recv;
    b->poll = loopback_poll;
    b->connected = loopback_connected;
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  TCP backend — non-blocking socket client
 * ═══════════════════════════════════════════════════════════════════════ */

static bool tcp_open(serial_backend_t* self)
{
    struct addrinfo hints, *res, *rp;
    char port_str[16];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", self->state.tcp.port);

    int err = getaddrinfo(self->state.tcp.host, port_str, &hints, &res);
    if (err != 0) {
        log_error("Serial TCP: getaddrinfo(%s:%s): %s",
                  self->state.tcp.host, port_str, gai_strerror(err));
        return false;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  /* Success */
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        log_error("Serial TCP: failed to connect to %s:%u",
                  self->state.tcp.host, self->state.tcp.port);
        return false;
    }

    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* Disable Nagle for low latency */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    self->state.tcp.sockfd = fd;
    log_info("Serial TCP: connected to %s:%u (fd=%d)",
             self->state.tcp.host, self->state.tcp.port, fd);
    return true;
}

static void tcp_close(serial_backend_t* self)
{
    if (self->state.tcp.sockfd >= 0) {
        close(self->state.tcp.sockfd);
        log_info("Serial TCP: disconnected");
        self->state.tcp.sockfd = -1;
    }
}

static bool tcp_send(serial_backend_t* self, uint8_t byte)
{
    if (self->state.tcp.sockfd < 0) return false;
    ssize_t n = safe_write(self->state.tcp.sockfd, &byte, 1);
    return n == 1;
}

static bool tcp_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.tcp.sockfd < 0) return false;
    ssize_t n = safe_read(self->state.tcp.sockfd, byte, 1);
    if (n == 1) return true;
    if (n == 0) {
        /* Connection closed by peer */
        log_info("Serial TCP: peer closed connection");
        close(self->state.tcp.sockfd);
        self->state.tcp.sockfd = -1;
    }
    return false;
}

static bool tcp_poll(serial_backend_t* self)
{
    if (self->state.tcp.sockfd < 0) return false;
    struct pollfd pfd = { .fd = self->state.tcp.sockfd, .events = POLLIN };
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

static bool tcp_connected(serial_backend_t* self)
{
    return self->state.tcp.sockfd >= 0;
}

serial_backend_t* serial_backend_tcp_create(const char* host, uint16_t port)
{
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    b->type = SERIAL_BACKEND_TCP;
    b->open = tcp_open;
    b->close = tcp_close;
    b->send = tcp_send;
    b->recv = tcp_recv;
    b->poll = tcp_poll;
    b->connected = tcp_connected;

    strncpy(b->state.tcp.host, host, sizeof(b->state.tcp.host) - 1);
    b->state.tcp.port = port;
    b->state.tcp.sockfd = -1;
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PTY backend — POSIX pseudo-terminal
 * ═══════════════════════════════════════════════════════════════════════ */

#if HAS_PTY

static bool pty_open(serial_backend_t* self)
{
    int master_fd;
    int slave_fd;

    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) {
        log_error("Serial PTY: openpty() failed: %s", strerror(errno));
        return false;
    }

    /* We only need the master side; close slave (user opens it externally) */
    char* name = ttyname(slave_fd);
    if (name) {
        strncpy(self->state.pty.slave_name, name,
                sizeof(self->state.pty.slave_name) - 1);
    }
    close(slave_fd);

    /* Set master non-blocking */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    self->state.pty.master_fd = master_fd;
    log_info("Serial PTY: opened %s (master fd=%d)", self->state.pty.slave_name, master_fd);
    return true;
}

static void pty_close(serial_backend_t* self)
{
    if (self->state.pty.master_fd >= 0) {
        close(self->state.pty.master_fd);
        log_info("Serial PTY: closed");
        self->state.pty.master_fd = -1;
    }
}

static bool pty_send(serial_backend_t* self, uint8_t byte)
{
    if (self->state.pty.master_fd < 0) return false;
    ssize_t n = safe_write(self->state.pty.master_fd, &byte, 1);
    return n == 1;
}

static bool pty_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.pty.master_fd < 0) return false;
    ssize_t n = safe_read(self->state.pty.master_fd, byte, 1);
    return n == 1;
}

static bool pty_poll(serial_backend_t* self)
{
    if (self->state.pty.master_fd < 0) return false;
    struct pollfd pfd = { .fd = self->state.pty.master_fd, .events = POLLIN };
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

static bool pty_connected(serial_backend_t* self)
{
    return self->state.pty.master_fd >= 0;
}

#endif /* HAS_PTY */

serial_backend_t* serial_backend_pty_create(void)
{
#if HAS_PTY
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    b->type = SERIAL_BACKEND_PTY;
    b->open = pty_open;
    b->close = pty_close;
    b->send = pty_send;
    b->recv = pty_recv;
    b->poll = pty_poll;
    b->connected = pty_connected;
    b->state.pty.master_fd = -1;
    return b;
#else
    log_error("Serial PTY: not supported on this platform");
    return NULL;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
 *  MODEM backend — TCP with AT command emulation (Hayes)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Ring-buffer helpers for modem RX buffer */
static void modem_rx_push(serial_backend_t* self, uint8_t byte)
{
    if (self->state.modem.rx_count >= SERIAL_MODEM_BUFSZ) return;
    self->state.modem.rx_buf[self->state.modem.rx_head] = byte;
    self->state.modem.rx_head = (self->state.modem.rx_head + 1) % SERIAL_MODEM_BUFSZ;
    self->state.modem.rx_count++;
}

static bool modem_rx_pop(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.modem.rx_count <= 0) return false;
    *byte = self->state.modem.rx_buf[self->state.modem.rx_tail];
    self->state.modem.rx_tail = (self->state.modem.rx_tail + 1) % SERIAL_MODEM_BUFSZ;
    self->state.modem.rx_count--;
    return true;
}

static void modem_rx_push_str(serial_backend_t* self, const char* s)
{
    while (*s) {
        modem_rx_push(self, (uint8_t)*s);
        s++;
    }
}

/* AT command processing */
static void modem_process_at(serial_backend_t* self, const char* cmd)
{
    /* Skip leading "AT" (case-insensitive) */
    if ((cmd[0] == 'A' || cmd[0] == 'a') &&
        (cmd[1] == 'T' || cmd[1] == 't')) {
        cmd += 2;
    }

    /* AT alone */
    if (cmd[0] == '\0') {
        modem_rx_push_str(self, "OK\r\n");
        return;
    }

    /* ATZ — reset */
    if (cmd[0] == 'Z' || cmd[0] == 'z') {
        self->state.modem.echo = true;
        self->state.modem.s0_rings = 0;
        self->state.modem.plus_count = 0;
        modem_rx_push_str(self, "OK\r\n");
        return;
    }

    /* ATE0 / ATE1 — echo control */
    if ((cmd[0] == 'E' || cmd[0] == 'e') && (cmd[1] == '0' || cmd[1] == '1')) {
        self->state.modem.echo = (cmd[1] == '1');
        modem_rx_push_str(self, "OK\r\n");
        return;
    }

    /* ATH / ATH0 — hangup */
    if (cmd[0] == 'H' || cmd[0] == 'h') {
        if (self->state.modem.sockfd >= 0) {
            close(self->state.modem.sockfd);
            self->state.modem.sockfd = -1;
            log_info("Serial Modem: hangup");
        }
        self->state.modem.mode = 0;  /* Back to command mode */
        modem_rx_push_str(self, "OK\r\n");
        return;
    }

    /* ATS0=N — auto-answer ring count */
    if ((cmd[0] == 'S' || cmd[0] == 's') && cmd[1] == '0' && cmd[2] == '=') {
        self->state.modem.s0_rings = atoi(&cmd[3]);
        modem_rx_push_str(self, "OK\r\n");
        return;
    }

    /* ATD[T] host:port — dial (TCP connect) */
    if (cmd[0] == 'D' || cmd[0] == 'd') {
        const char* target = cmd + 1;
        /* Skip optional T (tone dial) */
        if (*target == 'T' || *target == 't') target++;
        /* Skip leading spaces */
        while (*target == ' ') target++;

        /* Parse host:port */
        char dial_host[256] = {0};
        uint16_t dial_port = 23;  /* Default telnet port */
        const char* colon = strrchr(target, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - target);
            if (hlen >= sizeof(dial_host)) hlen = sizeof(dial_host) - 1;
            memcpy(dial_host, target, hlen);
            dial_host[hlen] = '\0';
            dial_port = (uint16_t)atoi(colon + 1);
        } else {
            strncpy(dial_host, target, sizeof(dial_host) - 1);
        }

        /* TCP connect */
        struct addrinfo hints, *res, *rp;
        char port_str[16];
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(port_str, sizeof(port_str), "%u", dial_port);

        int err = getaddrinfo(dial_host, port_str, &hints, &res);
        if (err != 0) {
            log_error("Serial Modem: ATD getaddrinfo(%s:%s): %s",
                      dial_host, port_str, gai_strerror(err));
            modem_rx_push_str(self, "NO CARRIER\r\n");
            return;
        }

        int fd = -1;
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);

        if (fd < 0) {
            log_error("Serial Modem: ATD failed to connect to %s:%u",
                      dial_host, dial_port);
            modem_rx_push_str(self, "NO CARRIER\r\n");
            return;
        }

        /* Set non-blocking + TCP_NODELAY */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        self->state.modem.sockfd = fd;
        self->state.modem.mode = 1;  /* Data mode */
        self->state.modem.plus_count = 0;
        log_info("Serial Modem: CONNECT to %s:%u (fd=%d)", dial_host, dial_port, fd);
        modem_rx_push_str(self, "CONNECT\r\n");
        return;
    }

    /* ATA — answer incoming connection */
    if (cmd[0] == 'A' || cmd[0] == 'a') {
        if (self->state.modem.listen_fd < 0) {
            modem_rx_push_str(self, "ERROR\r\n");
            return;
        }
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(self->state.modem.listen_fd,
                        (struct sockaddr*)&addr, &addrlen);
        if (fd < 0) {
            modem_rx_push_str(self, "ERROR\r\n");
            return;
        }
        /* Set non-blocking + TCP_NODELAY */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        self->state.modem.sockfd = fd;
        self->state.modem.mode = 1;  /* Data mode */
        self->state.modem.plus_count = 0;
        log_info("Serial Modem: CONNECT (accepted incoming, fd=%d)", fd);
        modem_rx_push_str(self, "CONNECT\r\n");
        return;
    }

    /* Unknown command */
    modem_rx_push_str(self, "ERROR\r\n");
}

static bool modem_open(serial_backend_t* self)
{
    /* Allocate 64KB ring buffers */
    self->state.modem.rx_buf = malloc(SERIAL_MODEM_BUFSZ);
    self->state.modem.tx_buf = malloc(SERIAL_MODEM_BUFSZ);
    if (!self->state.modem.rx_buf || !self->state.modem.tx_buf) {
        free(self->state.modem.rx_buf);
        free(self->state.modem.tx_buf);
        self->state.modem.rx_buf = NULL;
        self->state.modem.tx_buf = NULL;
        log_error("Serial Modem: failed to allocate buffers");
        return false;
    }
    self->state.modem.rx_head = 0;
    self->state.modem.rx_tail = 0;
    self->state.modem.rx_count = 0;
    self->state.modem.tx_head = 0;
    self->state.modem.tx_tail = 0;
    self->state.modem.tx_count = 0;

    self->state.modem.mode = 0;  /* Command mode */
    self->state.modem.cmd_len = 0;
    self->state.modem.echo = true;
    self->state.modem.plus_count = 0;
    self->state.modem.sockfd = -1;

    /* Server mode: create listening socket */
    if (self->state.modem.listening) {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) {
            log_error("Serial Modem: socket() failed: %s", strerror(errno));
            free(self->state.modem.rx_buf);
            free(self->state.modem.tx_buf);
            self->state.modem.rx_buf = NULL;
            self->state.modem.tx_buf = NULL;
            return false;
        }
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = INADDR_ANY;
        sa.sin_port = htons(self->state.modem.port);

        if (bind(lfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            log_error("Serial Modem: bind(:%u) failed: %s",
                      self->state.modem.port, strerror(errno));
            close(lfd);
            free(self->state.modem.rx_buf);
            free(self->state.modem.tx_buf);
            self->state.modem.rx_buf = NULL;
            self->state.modem.tx_buf = NULL;
            return false;
        }
        if (listen(lfd, 1) < 0) {
            log_error("Serial Modem: listen() failed: %s", strerror(errno));
            close(lfd);
            free(self->state.modem.rx_buf);
            free(self->state.modem.tx_buf);
            self->state.modem.rx_buf = NULL;
            self->state.modem.tx_buf = NULL;
            return false;
        }

        /* Set non-blocking for accept polling */
        int flags = fcntl(lfd, F_GETFL, 0);
        fcntl(lfd, F_SETFL, flags | O_NONBLOCK);

        self->state.modem.listen_fd = lfd;
        log_info("Serial Modem: listening on port %u (fd=%d)",
                 self->state.modem.port, lfd);
    }

    log_info("Serial Modem: backend opened (mode=command, echo=on)");
    return true;
}

static void modem_close(serial_backend_t* self)
{
    if (self->state.modem.sockfd >= 0) {
        close(self->state.modem.sockfd);
        self->state.modem.sockfd = -1;
    }
    if (self->state.modem.listen_fd >= 0) {
        close(self->state.modem.listen_fd);
        self->state.modem.listen_fd = -1;
    }
    free(self->state.modem.rx_buf);
    free(self->state.modem.tx_buf);
    self->state.modem.rx_buf = NULL;
    self->state.modem.tx_buf = NULL;
    log_info("Serial Modem: backend closed");
}

static bool modem_send(serial_backend_t* self, uint8_t byte)
{
    /* Data mode: send to socket, detect +++ escape with guard time.
     * Hayes spec: 1 second silence, then "+++", then 1 second silence.
     * We use last_data_time to track silence periods. At 1 MHz CPU,
     * 1 second = 1000000 cycles. We approximate with a counter that
     * increments each time modem_send is called (baud-rate limited). */
    if (self->state.modem.mode == 1) {
        /* +++ escape detection with guard time.
         * Hayes spec: silence → +++ → silence → command mode.
         * Guard satisfied if last_data_time >= 50 (baud-rate polls). */
        if (byte == '+' && self->state.modem.last_data_time >= 50) {
            self->state.modem.plus_count++;
            if (self->state.modem.plus_count >= 3) {
                self->state.modem.mode = 0;
                self->state.modem.plus_count = 0;
                self->state.modem.cmd_len = 0;
                self->state.modem.last_data_time = 0;
                log_info("Serial Modem: +++ escape -> command mode");
                modem_rx_push_str(self, "\r\nOK\r\n");
                return true;
            }
            self->state.modem.last_data_time = 0;
            return true;
        }

        /* Non-escape or '+' without guard: flush pending '+' and send data */
        if (self->state.modem.sockfd >= 0 && self->state.modem.plus_count > 0) {
            uint8_t buf[3];
            for (int i = 0; i < self->state.modem.plus_count && i < 3; i++)
                buf[i] = '+';
            (void)safe_write(self->state.modem.sockfd, buf,
                        (size_t)self->state.modem.plus_count);
        }
        self->state.modem.plus_count = 0;
        self->state.modem.last_data_time = 0;

        if (self->state.modem.sockfd >= 0) {
            ssize_t n = safe_write(self->state.modem.sockfd, &byte, 1);
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                log_info("Serial Modem: write error, dropping carrier");
                close(self->state.modem.sockfd);
                self->state.modem.sockfd = -1;
                self->state.modem.mode = 0;
                modem_rx_push_str(self, "\r\nNO CARRIER\r\n");
                return false;
            }
        }
        return true;
    }

    /* Command mode: accumulate AT command */
    if (self->state.modem.echo) {
        modem_rx_push(self, byte);
    }

    if (byte == '\r' || byte == '\n') {
        if (self->state.modem.cmd_len > 0) {
            self->state.modem.cmd_buf[self->state.modem.cmd_len] = '\0';
            if (self->state.modem.echo) {
                modem_rx_push_str(self, "\r\n");
            }
            modem_process_at(self, self->state.modem.cmd_buf);
            self->state.modem.cmd_len = 0;
        }
    } else if (byte == '\b' || byte == 127) {
        /* Backspace */
        if (self->state.modem.cmd_len > 0) {
            self->state.modem.cmd_len--;
        }
    } else {
        if (self->state.modem.cmd_len < (int)sizeof(self->state.modem.cmd_buf) - 1) {
            self->state.modem.cmd_buf[self->state.modem.cmd_len++] = (char)byte;
        }
    }
    return true;
}

static bool modem_recv(serial_backend_t* self, uint8_t* byte)
{
    /* Increment silence counter for +++ guard time detection */
    self->state.modem.last_data_time++;

    /* In data mode, bulk-read from socket into rx_buf */
    if (self->state.modem.mode == 1 && self->state.modem.sockfd >= 0) {
        uint8_t tmp[256];
        ssize_t n = safe_read(self->state.modem.sockfd, tmp, sizeof(tmp));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                modem_rx_push(self, tmp[i]);
            }
        } else if (n == 0) {
            /* Peer closed */
            log_info("Serial Modem: peer closed connection");
            close(self->state.modem.sockfd);
            self->state.modem.sockfd = -1;
            self->state.modem.mode = 0;
            modem_rx_push_str(self, "\r\nNO CARRIER\r\n");
        }
        /* n < 0: EAGAIN/EWOULDBLOCK — no data available, that's fine */
    }

    return modem_rx_pop(self, byte);
}

static bool modem_poll(serial_backend_t* self)
{
    /* Check if there's data in the rx buffer */
    if (self->state.modem.rx_count > 0) return true;

    /* In data mode, check socket for incoming data */
    if (self->state.modem.mode == 1 && self->state.modem.sockfd >= 0) {
        struct pollfd pfd = { .fd = self->state.modem.sockfd, .events = POLLIN };
        return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
    }

    return false;
}

static bool modem_connected(serial_backend_t* self)
{
    return self->state.modem.mode == 1 && self->state.modem.sockfd >= 0;
}

serial_backend_t* serial_backend_modem_create(const char* host, uint16_t port, bool listen_mode)
{
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    b->type = SERIAL_BACKEND_MODEM;
    b->open = modem_open;
    b->close = modem_close;
    b->send = modem_send;
    b->recv = modem_recv;
    b->poll = modem_poll;
    b->connected = modem_connected;

    if (host) {
        strncpy(b->state.modem.host, host, sizeof(b->state.modem.host) - 1);
    }
    b->state.modem.port = port;
    b->state.modem.sockfd = -1;
    b->state.modem.listen_fd = -1;
    b->state.modem.listening = listen_mode;
    b->state.modem.echo = true;
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  COM backend — Real serial port via termios (Linux only)
 * ═══════════════════════════════════════════════════════════════════════ */

#if HAS_COM

static speed_t com_baud_to_speed(int baud)
{
    switch (baud) {
        case 50:     return B50;
        case 75:     return B75;
        case 110:    return B110;
        case 134:    return B134;
        case 150:    return B150;
        case 200:    return B200;
        case 300:    return B300;
        case 600:    return B600;
        case 1200:   return B1200;
        case 1800:   return B1800;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B9600;
    }
}

static bool com_open(serial_backend_t* self)
{
    int fd = open(self->state.com.device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        log_error("Serial COM: open(%s) failed: %s",
                  self->state.com.device, strerror(errno));
        return false;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        log_error("Serial COM: tcgetattr failed: %s", strerror(errno));
        close(fd);
        return false;
    }

    /* Save original termios for restore on close */
    if (sizeof(struct termios) <= sizeof(self->state.com.orig_termios)) {
        memcpy(self->state.com.orig_termios, &tty, sizeof(struct termios));
        self->state.com.has_orig = true;
    }

    /* Baud rate */
    speed_t spd = com_baud_to_speed(self->state.com.baud);
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    /* Control flags: CLOCAL (ignore modem control) + CREAD (enable receiver) */
    tty.c_cflag |= (CLOCAL | CREAD);

    /* Data bits */
    tty.c_cflag &= ~CSIZE;
    switch (self->state.com.databits) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        case 8: default: tty.c_cflag |= CS8; break;
    }

    /* Parity */
    switch (self->state.com.parity) {
        case 'E': case 'e':
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
        case 'O': case 'o':
            tty.c_cflag |= (PARENB | PARODD);
            break;
        case 'N': case 'n': default:
            tty.c_cflag &= ~PARENB;
            break;
    }

    /* Stop bits */
    if (self->state.com.stopbits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }

    /* Raw mode: no echo, no canonical, no signals, no special processing */
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | INLCR | ICRNL | IGNCR);
    tty.c_oflag &= ~OPOST;

    /* Non-blocking read: return immediately */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        log_error("Serial COM: tcsetattr failed: %s", strerror(errno));
        close(fd);
        return false;
    }

    /* Flush any stale data */
    tcflush(fd, TCIOFLUSH);

    self->state.com.fd = fd;
    log_info("Serial COM: opened %s at %d,%d,%c,%d (fd=%d)",
             self->state.com.device, self->state.com.baud,
             self->state.com.databits, self->state.com.parity,
             self->state.com.stopbits, fd);
    return true;
}

static void com_close(serial_backend_t* self)
{
    if (self->state.com.fd >= 0) {
        tcdrain(self->state.com.fd);
        /* Restore original termios settings */
        if (self->state.com.has_orig) {
            struct termios orig;
            memcpy(&orig, self->state.com.orig_termios, sizeof(struct termios));
            tcsetattr(self->state.com.fd, TCSANOW, &orig);
        }
        close(self->state.com.fd);
        log_info("Serial COM: closed %s", self->state.com.device);
        self->state.com.fd = -1;
    }
}

static bool com_send(serial_backend_t* self, uint8_t byte)
{
    if (self->state.com.fd < 0) return false;
    ssize_t n = safe_write(self->state.com.fd, &byte, 1);
    return n == 1;
}

static bool com_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.com.fd < 0) return false;
    ssize_t n = safe_read(self->state.com.fd, byte, 1);
    return n == 1;
}

static bool com_poll(serial_backend_t* self)
{
    if (self->state.com.fd < 0) return false;
    int bytes_avail = 0;
    if (ioctl(self->state.com.fd, FIONREAD, &bytes_avail) < 0) return false;
    return bytes_avail > 0;
}

static bool com_connected(serial_backend_t* self)
{
    return self->state.com.fd >= 0;
}

#endif /* HAS_COM */

serial_backend_t* serial_backend_com_create(const char* config)
{
#if HAS_COM
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    b->type = SERIAL_BACKEND_COM;
    b->open = com_open;
    b->close = com_close;
    b->send = com_send;
    b->recv = com_recv;
    b->poll = com_poll;
    b->connected = com_connected;
    b->state.com.fd = -1;

    /* Default values */
    b->state.com.baud = 9600;
    b->state.com.databits = 8;
    b->state.com.parity = 'N';
    b->state.com.stopbits = 1;
    strncpy(b->state.com.device, "/dev/ttyUSB0", sizeof(b->state.com.device) - 1);

    /* Parse config: "baud,databits,parity,stopbits,device" */
    if (config && config[0]) {
        char tmp[512];
        strncpy(tmp, config, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char* tok = strtok(tmp, ",");
        if (tok) { b->state.com.baud = atoi(tok); tok = strtok(NULL, ","); }
        if (tok) { b->state.com.databits = atoi(tok); tok = strtok(NULL, ","); }
        if (tok) { b->state.com.parity = tok[0]; tok = strtok(NULL, ","); }
        if (tok) { b->state.com.stopbits = atoi(tok); tok = strtok(NULL, ","); }
        if (tok) { strncpy(b->state.com.device, tok, sizeof(b->state.com.device) - 1); }
    }

    return b;
#else
    (void)config;
    log_error("Serial COM: not supported on this platform");
    return NULL;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
 *  DIGITELEC DTL 2000 backend — V23/V21 modem emulation
 *
 *  The Digitelec DTL 2000 (1984, ~1490 FF) was an autonomous external
 *  modem supporting V23 (1200/75 baud, Minitel) and V21 (300/300 baud).
 *  It connected to the ORIC via a card on the expansion bus and
 *  communicated with the ACIA via standard RS232 signals.
 *
 *  Key behaviors emulated:
 *  - Internal buffering (the modem had its own RAM)
 *  - CTS flow control: modem deasserts CTS when RX buffer is near full,
 *    preventing the remote end from overrunning the ORIC
 *  - DCD: reflects TCP connection state (carrier = connected)
 *  - DTR: ORIC asserts DTR to "dial" (trigger TCP connect)
 *    ORIC deasserts DTR to "hang up" (close TCP)
 *  - No AT commands (the Digitelec predates the Hayes standard)
 * ═══════════════════════════════════════════════════════════════════════ */

#include "io/acia6551.h"  /* For acia_set_dcd/cts */

static bool digitelec_open(serial_backend_t* self)
{
    self->state.digitelec.rx_head = 0;
    self->state.digitelec.rx_tail = 0;
    self->state.digitelec.rx_count = 0;
    self->state.digitelec.tx_head = 0;
    self->state.digitelec.tx_tail = 0;
    self->state.digitelec.tx_count = 0;

    self->state.digitelec.carrier = false;
    self->state.digitelec.dtr_was_on = false;
    self->state.digitelec.sockfd = -1;

    /* Flow control: CTS off when RX buffer > 400 bytes, on when < 256 */
    self->state.digitelec.cts_high_water = 400;
    self->state.digitelec.cts_low_water = 256;
    self->state.digitelec.cts_active = true;

    /* Drive initial signal lines on ACIA */
    acia6551_t* acia = (acia6551_t*)self->state.digitelec.acia_ptr;
    if (acia) {
        acia_set_dcd(acia, false);   /* No carrier yet */
        acia_set_dsr(acia, true);    /* Modem ready */
        acia_set_cts(acia, true);    /* Clear to send */
    }

    log_info("Digitelec DTL 2000: modem ready (V23 1200/75, host=%s:%u)",
             self->state.digitelec.host, self->state.digitelec.port);
    return true;
}

static void digitelec_close(serial_backend_t* self)
{
    if (self->state.digitelec.sockfd >= 0) {
        close(self->state.digitelec.sockfd);
        self->state.digitelec.sockfd = -1;
    }
    self->state.digitelec.carrier = false;

    acia6551_t* acia = (acia6551_t*)self->state.digitelec.acia_ptr;
    if (acia) {
        acia_set_dcd(acia, false);
    }

    log_info("Digitelec DTL 2000: modem closed");
}

/**
 * @brief Attempt TCP connection (triggered by DTR rising edge)
 */
static bool digitelec_connect(serial_backend_t* self)
{
    struct addrinfo hints, *res, *rp;
    char port_str[16];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%u", self->state.digitelec.port);

    int err = getaddrinfo(self->state.digitelec.host, port_str, &hints, &res);
    if (err != 0) {
        log_error("Digitelec DTL 2000: connect failed (%s)", gai_strerror(err));
        return false;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        log_error("Digitelec DTL 2000: no carrier (%s:%u)",
                  self->state.digitelec.host, self->state.digitelec.port);
        return false;
    }

    /* Non-blocking + TCP_NODELAY */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    self->state.digitelec.sockfd = fd;
    self->state.digitelec.carrier = true;

    /* Drive DCD on ACIA */
    acia6551_t* acia = (acia6551_t*)self->state.digitelec.acia_ptr;
    if (acia) {
        acia_set_dcd(acia, true);  /* Carrier detected */
    }

    log_info("Digitelec DTL 2000: CARRIER DETECT (%s:%u, fd=%d)",
             self->state.digitelec.host, self->state.digitelec.port, fd);
    return true;
}

/**
 * @brief Disconnect (triggered by DTR falling edge)
 */
static void digitelec_disconnect(serial_backend_t* self)
{
    if (self->state.digitelec.sockfd >= 0) {
        close(self->state.digitelec.sockfd);
        self->state.digitelec.sockfd = -1;
    }
    self->state.digitelec.carrier = false;

    /* Flush buffers on hangup */
    self->state.digitelec.rx_head = 0;
    self->state.digitelec.rx_tail = 0;
    self->state.digitelec.rx_count = 0;
    self->state.digitelec.tx_head = 0;
    self->state.digitelec.tx_tail = 0;
    self->state.digitelec.tx_count = 0;

    acia6551_t* acia = (acia6551_t*)self->state.digitelec.acia_ptr;
    if (acia) {
        acia_set_dcd(acia, false);  /* No carrier */
        acia_set_cts(acia, true);   /* Reset CTS */
    }
    self->state.digitelec.cts_active = true;

    log_info("Digitelec DTL 2000: NO CARRIER (hangup)");
}

/**
 * @brief Check DTR edge for connect/disconnect (called from send and poll)
 */
static void digitelec_check_dtr(serial_backend_t* self)
{
    acia6551_t* acia = (acia6551_t*)self->state.digitelec.acia_ptr;
    if (!acia) return;
    bool dtr_on = (acia->command & ACIA_CMD_DTR) != 0;
    if (dtr_on && !self->state.digitelec.dtr_was_on) {
        if (!self->state.digitelec.carrier) digitelec_connect(self);
    } else if (!dtr_on && self->state.digitelec.dtr_was_on) {
        if (self->state.digitelec.carrier) digitelec_disconnect(self);
    }
    self->state.digitelec.dtr_was_on = dtr_on;
}

static bool digitelec_send(serial_backend_t* self, uint8_t byte)
{
    digitelec_check_dtr(self);

    if (!self->state.digitelec.carrier || self->state.digitelec.sockfd < 0)
        return false;

    /* Send directly to TCP (modem transmits immediately on the "line") */
    ssize_t n = safe_write(self->state.digitelec.sockfd, &byte, 1);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_info("Digitelec DTL 2000: write error, carrier lost");
        digitelec_disconnect(self);
        return false;
    }
    return true;
}

static bool digitelec_recv(serial_backend_t* self, uint8_t* byte)
{
    /* Bulk-read from TCP socket into modem's internal RX buffer */
    if (self->state.digitelec.carrier && self->state.digitelec.sockfd >= 0) {
        while (self->state.digitelec.rx_count < 512) {
            uint8_t tmp[64];
            int space = 512 - self->state.digitelec.rx_count;
            if (space > (int)sizeof(tmp)) space = (int)sizeof(tmp);
            ssize_t n = safe_read(self->state.digitelec.sockfd, tmp, (size_t)space);
            if (n > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    self->state.digitelec.rx_buf[self->state.digitelec.rx_head] = tmp[i];
                    self->state.digitelec.rx_head = (self->state.digitelec.rx_head + 1) % 512;
                    self->state.digitelec.rx_count++;
                }
            } else if (n == 0) {
                log_info("Digitelec DTL 2000: peer closed, carrier lost");
                digitelec_disconnect(self);
                break;
            } else {
                break;  /* EAGAIN */
            }
        }

        /* Flow control: manage CTS based on buffer level */
        acia6551_t* acia = (acia6551_t*)self->state.digitelec.acia_ptr;
        if (acia) {
            if (self->state.digitelec.cts_active &&
                self->state.digitelec.rx_count >= self->state.digitelec.cts_high_water) {
                /* Buffer near full → deassert CTS to pause remote */
                self->state.digitelec.cts_active = false;
                acia_set_cts(acia, false);
            } else if (!self->state.digitelec.cts_active &&
                       self->state.digitelec.rx_count <= self->state.digitelec.cts_low_water) {
                /* Buffer drained → reassert CTS */
                self->state.digitelec.cts_active = true;
                acia_set_cts(acia, true);
            }
        }
    }

    /* Pop one byte from modem RX buffer */
    if (self->state.digitelec.rx_count <= 0)
        return false;
    *byte = self->state.digitelec.rx_buf[self->state.digitelec.rx_tail];
    self->state.digitelec.rx_tail = (self->state.digitelec.rx_tail + 1) % 512;
    self->state.digitelec.rx_count--;
    return true;
}

static bool digitelec_poll(serial_backend_t* self)
{
    digitelec_check_dtr(self);

    /* Data available in modem's internal buffer */
    if (self->state.digitelec.rx_count > 0)
        return true;

    /* Check TCP socket for new data */
    if (self->state.digitelec.carrier && self->state.digitelec.sockfd >= 0) {
        struct pollfd pfd = { .fd = self->state.digitelec.sockfd, .events = POLLIN };
        return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
    }
    return false;
}

static bool digitelec_connected(serial_backend_t* self)
{
    return self->state.digitelec.carrier;
}

serial_backend_t* serial_backend_digitelec_create(const char* host, uint16_t port, void* acia)
{
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    b->type = SERIAL_BACKEND_DIGITELEC;
    b->open = digitelec_open;
    b->close = digitelec_close;
    b->send = digitelec_send;
    b->recv = digitelec_recv;
    b->poll = digitelec_poll;
    b->connected = digitelec_connected;

    if (host) {
        strncpy(b->state.digitelec.host, host, sizeof(b->state.digitelec.host) - 1);
    }
    b->state.digitelec.port = port;
    b->state.digitelec.sockfd = -1;
    b->state.digitelec.acia_ptr = acia;
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  FILE backend — deterministic replay (RX) / capture (TX)
 * ═══════════════════════════════════════════════════════════════════════ */

static bool file_open(serial_backend_t* self)
{
    self->state.file.peeked = -1;
    self->state.file.in = NULL;
    self->state.file.out = NULL;

    if (self->state.file.in_path[0]) {
        FILE* f = fopen(self->state.file.in_path, "rb");
        if (!f) {
            log_error("Serial FILE: open(%s) for replay failed: %s",
                      self->state.file.in_path, strerror(errno));
            return false;
        }
        self->state.file.in = f;
    }
    if (self->state.file.out_path[0]) {
        FILE* f = fopen(self->state.file.out_path, "wb");
        if (!f) {
            log_error("Serial FILE: open(%s) for capture failed: %s",
                      self->state.file.out_path, strerror(errno));
            if (self->state.file.in) { fclose((FILE*)self->state.file.in); self->state.file.in = NULL; }
            return false;
        }
        self->state.file.out = f;
    }

    log_info("Serial FILE: replay=%s capture=%s",
             self->state.file.in_path[0]  ? self->state.file.in_path  : "(none)",
             self->state.file.out_path[0] ? self->state.file.out_path : "(none)");
    return true;
}

static void file_close(serial_backend_t* self)
{
    if (self->state.file.in)  { fclose((FILE*)self->state.file.in);  self->state.file.in  = NULL; }
    if (self->state.file.out) { fflush((FILE*)self->state.file.out);
                                fclose((FILE*)self->state.file.out); self->state.file.out = NULL; }
}

static bool file_send(serial_backend_t* self, uint8_t byte)
{
    if (self->state.file.out) {
        fputc(byte, (FILE*)self->state.file.out);
        /* Flush eagerly so captures are observable mid-run (and survive a
         * cycle-limited headless exit that never calls close()). */
        fflush((FILE*)self->state.file.out);
    }
    return true;  /* TX is always accepted (discarded if no capture file) */
}

static bool file_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.file.peeked >= 0) {
        *byte = (uint8_t)self->state.file.peeked;
        self->state.file.peeked = -1;
        return true;
    }
    if (!self->state.file.in) return false;
    int c = fgetc((FILE*)self->state.file.in);
    if (c == EOF) return false;
    *byte = (uint8_t)c;
    return true;
}

static bool file_poll(serial_backend_t* self)
{
    if (self->state.file.peeked >= 0) return true;
    if (!self->state.file.in) return false;
    int c = fgetc((FILE*)self->state.file.in);
    if (c == EOF) return false;
    self->state.file.peeked = c;  /* one-byte lookahead → accurate poll */
    return true;
}

static bool file_connected(serial_backend_t* self)
{
    (void)self;
    return true;  /* the "line" is the file(s); always present */
}

serial_backend_t* serial_backend_file_create(const char* in_path, const char* out_path)
{
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    b->type = SERIAL_BACKEND_FILE;
    b->open = file_open;
    b->close = file_close;
    b->send = file_send;
    b->recv = file_recv;
    b->poll = file_poll;
    b->connected = file_connected;

    if (in_path) {
        strncpy(b->state.file.in_path, in_path, sizeof(b->state.file.in_path) - 1);
    }
    if (out_path) {
        strncpy(b->state.file.out_path, out_path, sizeof(b->state.file.out_path) - 1);
    }
    b->state.file.peeked = -1;
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Common destroy
 * ═══════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════
 *  Real-time host MIDI backend — ALSA (Linux) / CoreMIDI (macOS) / WinMM (Win)
 *  All three implement the same vtable (open/close/send/recv/poll/connected)
 *  and the same factory serial_backend_midi_create(). Only the host platform's
 *  branch is compiled.
 * ═══════════════════════════════════════════════════════════════════════ */

#ifdef MIDI_BACKEND_ALSA

/* Drain any pending input events from the sequencer, decoding each into raw
 * MIDI bytes pushed onto the RX ring. Non-blocking (seq opened NONBLOCK). */
static void midi_pump_input(serial_backend_t* self)
{
    snd_seq_t* seq = (snd_seq_t*)self->state.midi.seq;
    snd_midi_event_t* dec = (snd_midi_event_t*)self->state.midi.decoder;
    if (!seq || !dec) return;

    snd_seq_event_t* ev = NULL;
    while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
        unsigned char buf[64];
        long n = snd_midi_event_decode(dec, buf, sizeof(buf), ev);
        for (long i = 0; i < n; i++) {
            if (self->state.midi.rx_count >= (int)sizeof(self->state.midi.rx_buf))
                break;  /* ring full — drop (synth can't keep up; rare) */
            self->state.midi.rx_buf[self->state.midi.rx_tail] = buf[i];
            self->state.midi.rx_tail =
                (self->state.midi.rx_tail + 1) % (int)sizeof(self->state.midi.rx_buf);
            self->state.midi.rx_count++;
        }
    }
}

static bool midi_open(serial_backend_t* self)
{
    snd_seq_t* seq = NULL;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        log_error("MIDI: cannot open ALSA sequencer (is snd-seq loaded?)");
        return false;
    }
    snd_seq_set_client_name(seq, "Phosphoric MIDI");

    int port = snd_seq_create_simple_port(
        seq, "Mageco MIDI",
        SND_SEQ_PORT_CAP_READ  | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) {
        log_error("MIDI: cannot create sequencer port");
        snd_seq_close(seq);
        return false;
    }

    /* Encoder (TX byte→event) and decoder (RX event→byte). A 256-byte buffer
     * comfortably holds any single MIDI message (incl. small SysEx fragments).
     * Running status is left enabled on encode (faithful to the wire). */
    snd_midi_event_t *enc = NULL, *dec = NULL;
    if (snd_midi_event_new(256, &enc) < 0 || snd_midi_event_new(256, &dec) < 0) {
        log_error("MIDI: cannot allocate MIDI event parser");
        if (enc) snd_midi_event_free(enc);
        snd_seq_delete_simple_port(seq, port);
        snd_seq_close(seq);
        return false;
    }
    snd_midi_event_init(enc);
    snd_midi_event_init(dec);
    snd_midi_event_no_status(dec, 1);  /* decode: always emit explicit status */

    self->state.midi.seq     = seq;
    self->state.midi.encoder = enc;
    self->state.midi.decoder = dec;
    self->state.midi.port    = port;
    self->state.midi.rx_head = self->state.midi.rx_tail = self->state.midi.rx_count = 0;

    int client = snd_seq_client_id(seq);
    log_info("MIDI: ALSA port ready — %d:%d \"Phosphoric MIDI:Mageco MIDI\" "
             "(connect with: aconnect %d:%d <synth>)", client, port, client, port);

    /* Optional auto-connect to a target address (e.g. "128:0" or "FLUID"). */
    if (self->state.midi.target[0]) {
        snd_seq_addr_t addr;
        if (snd_seq_parse_address(seq, &addr, self->state.midi.target) == 0) {
            if (snd_seq_connect_to(seq, port, addr.client, addr.port) == 0)
                log_info("MIDI: connected OUT → %d:%d", addr.client, addr.port);
            /* RX from the same address is best-effort (keyboards/controllers). */
            snd_seq_connect_from(seq, port, addr.client, addr.port);
        } else {
            log_warning("MIDI: cannot resolve target '%s' (use aconnect manually)",
                        self->state.midi.target);
        }
    }
    return true;
}

static void midi_close(serial_backend_t* self)
{
    if (self->state.midi.encoder) {
        snd_midi_event_free((snd_midi_event_t*)self->state.midi.encoder);
        self->state.midi.encoder = NULL;
    }
    if (self->state.midi.decoder) {
        snd_midi_event_free((snd_midi_event_t*)self->state.midi.decoder);
        self->state.midi.decoder = NULL;
    }
    if (self->state.midi.seq) {
        snd_seq_close((snd_seq_t*)self->state.midi.seq);
        self->state.midi.seq = NULL;
    }
}

static bool midi_send(serial_backend_t* self, uint8_t byte)
{
    snd_seq_t* seq = (snd_seq_t*)self->state.midi.seq;
    snd_midi_event_t* enc = (snd_midi_event_t*)self->state.midi.encoder;
    if (!seq || !enc) return false;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    /* Feed one byte; a positive return means a complete event is assembled. */
    long r = snd_midi_event_encode_byte(enc, byte, &ev);
    if (r > 0) {
        snd_seq_ev_set_source(&ev, self->state.midi.port);
        snd_seq_ev_set_subs(&ev);     /* to all subscribers */
        snd_seq_ev_set_direct(&ev);   /* deliver now (real-time) */
        snd_seq_event_output_direct(seq, &ev);
    }
    return true;  /* byte always accepted (queued in the encoder) */
}

static bool midi_recv(serial_backend_t* self, uint8_t* byte)
{
    midi_pump_input(self);
    if (self->state.midi.rx_count <= 0) return false;
    *byte = self->state.midi.rx_buf[self->state.midi.rx_head];
    self->state.midi.rx_head =
        (self->state.midi.rx_head + 1) % (int)sizeof(self->state.midi.rx_buf);
    self->state.midi.rx_count--;
    return true;
}

static bool midi_poll(serial_backend_t* self)
{
    midi_pump_input(self);
    return self->state.midi.rx_count > 0;
}

static bool midi_connected(serial_backend_t* self)
{
    return self->state.midi.seq != NULL;
}

#elif defined(MIDI_BACKEND_COREMIDI)

/* ── macOS CoreMIDI ─────────────────────────────────────────────────────
 * NOTE: written to the documented CoreMIDI API but NOT compiled/verified on
 * this Linux host. The byte↔packet model mirrors the ALSA branch. */

typedef struct {
    MIDIClientRef   client;
    MIDIPortRef     out_port;   /* our output (Oric → host) */
    MIDIPortRef     in_port;    /* our input  (host → Oric) */
    MIDIEndpointRef dst;        /* destination we send to (target or virtual) */
    MIDIEndpointRef vsrc;       /* virtual source we expose (Oric MIDI OUT) */
    MIDIEndpointRef vdst;       /* virtual destination (Oric MIDI IN) */
} coremidi_state_t;

/* CoreMIDI read callback: incoming packets → RX ring (host → Oric). */
static void coremidi_read_cb(const MIDIPacketList* pktlist, void* refCon, void* connRefCon)
{
    (void)connRefCon;
    serial_backend_t* self = (serial_backend_t*)refCon;
    const MIDIPacket* pkt = &pktlist->packet[0];
    for (unsigned i = 0; i < pktlist->numPackets; i++) {
        for (unsigned j = 0; j < pkt->length; j++) {
            if (self->state.midi.rx_count >= (int)sizeof(self->state.midi.rx_buf)) break;
            self->state.midi.rx_buf[self->state.midi.rx_tail] = pkt->data[j];
            self->state.midi.rx_tail =
                (self->state.midi.rx_tail + 1) % (int)sizeof(self->state.midi.rx_buf);
            self->state.midi.rx_count++;
        }
        pkt = MIDIPacketNext(pkt);
    }
}

static bool midi_open(serial_backend_t* self)
{
    coremidi_state_t* st = calloc(1, sizeof(*st));
    if (!st) return false;

    if (MIDIClientCreate(CFSTR("Phosphoric MIDI"), NULL, NULL, &st->client) != noErr) {
        log_error("MIDI: MIDIClientCreate failed");
        free(st);
        return false;
    }
    MIDIOutputPortCreate(st->client, CFSTR("Mageco OUT"), &st->out_port);
    MIDIInputPortCreate(st->client, CFSTR("Mageco IN"), coremidi_read_cb, self, &st->in_port);
    /* Virtual endpoints so the Oric appears in the host MIDI graph. */
    MIDISourceCreate(st->client, CFSTR("Phosphoric MIDI"), &st->vsrc);
    MIDIDestinationCreate(st->client, CFSTR("Phosphoric MIDI"), coremidi_read_cb, self, &st->vdst);

    /* Optional target: connect to destination index 0 / by name match. */
    st->dst = 0;
    if (self->state.midi.target[0]) {
        ItemCount n = MIDIGetNumberOfDestinations();
        for (ItemCount i = 0; i < n; i++) {
            MIDIEndpointRef d = MIDIGetDestination(i);
            CFStringRef nm = NULL;
            MIDIObjectGetStringProperty(d, kMIDIPropertyName, &nm);
            char buf[128] = {0};
            if (nm) { CFStringGetCString(nm, buf, sizeof(buf), kCFStringEncodingUTF8); CFRelease(nm); }
            if (strstr(buf, self->state.midi.target)) { st->dst = d; break; }
        }
    }
    self->state.midi.seq = st;
    self->state.midi.rx_head = self->state.midi.rx_tail = self->state.midi.rx_count = 0;
    log_info("MIDI: CoreMIDI port ready (\"Phosphoric MIDI\")");
    return true;
}

static void midi_close(serial_backend_t* self)
{
    coremidi_state_t* st = (coremidi_state_t*)self->state.midi.seq;
    if (!st) return;
    if (st->client) MIDIClientDispose(st->client);
    free(st);
    self->state.midi.seq = NULL;
}

static bool midi_send(serial_backend_t* self, uint8_t byte)
{
    coremidi_state_t* st = (coremidi_state_t*)self->state.midi.seq;
    if (!st) return false;
    Byte buffer[256];
    MIDIPacketList* pl = (MIDIPacketList*)buffer;
    MIDIPacket* pkt = MIDIPacketListInit(pl);
    pkt = MIDIPacketListAdd(pl, sizeof(buffer), pkt, 0, 1, &byte);
    if (st->dst) MIDISend(st->out_port, st->dst, pl);
    MIDIReceived(st->vsrc, pl);   /* also emit on our virtual source */
    return true;
}

static bool midi_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.midi.rx_count <= 0) return false;
    *byte = self->state.midi.rx_buf[self->state.midi.rx_head];
    self->state.midi.rx_head =
        (self->state.midi.rx_head + 1) % (int)sizeof(self->state.midi.rx_buf);
    self->state.midi.rx_count--;
    return true;
}

static bool midi_poll(serial_backend_t* self) { return self->state.midi.rx_count > 0; }
static bool midi_connected(serial_backend_t* self) { return self->state.midi.seq != NULL; }

#elif defined(MIDI_BACKEND_WINMM)

/* ── Windows WinMM ──────────────────────────────────────────────────────
 * NOTE: written to the documented WinMM API but NOT compiled/verified on this
 * Linux host. Opens the default MIDI out/in devices (or by index in target). */

typedef struct {
    HMIDIOUT out;
    HMIDIIN  in;
    bool     have_in;
} winmm_state_t;

static void CALLBACK winmm_in_cb(HMIDIIN h, UINT msg, DWORD_PTR inst,
                                 DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h; (void)p2;
    if (msg != MIM_DATA) return;
    serial_backend_t* self = (serial_backend_t*)inst;
    uint8_t b[3] = { (uint8_t)(p1 & 0xFF), (uint8_t)((p1 >> 8) & 0xFF),
                     (uint8_t)((p1 >> 16) & 0xFF) };
    uint8_t status = b[0];
    int n = (status >= 0xC0 && status <= 0xDF) ? 2 :
            (status >= 0xF0) ? 1 : 3;
    for (int i = 0; i < n; i++) {
        if (self->state.midi.rx_count >= (int)sizeof(self->state.midi.rx_buf)) break;
        self->state.midi.rx_buf[self->state.midi.rx_tail] = b[i];
        self->state.midi.rx_tail =
            (self->state.midi.rx_tail + 1) % (int)sizeof(self->state.midi.rx_buf);
        self->state.midi.rx_count++;
    }
}

static bool midi_open(serial_backend_t* self)
{
    winmm_state_t* st = calloc(1, sizeof(*st));
    if (!st) return false;
    UINT dev = (self->state.midi.target[0]) ? (UINT)atoi(self->state.midi.target)
                                            : (UINT)MIDI_MAPPER;
    if (midiOutOpen(&st->out, dev, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        log_error("MIDI: midiOutOpen failed");
        free(st);
        return false;
    }
    if (midiInGetNumDevs() > 0 &&
        midiInOpen(&st->in, 0, (DWORD_PTR)winmm_in_cb, (DWORD_PTR)self,
                   CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
        midiInStart(st->in);
        st->have_in = true;
    }
    self->state.midi.seq = st;
    self->state.midi.rx_head = self->state.midi.rx_tail = self->state.midi.rx_count = 0;
    log_info("MIDI: WinMM port ready");
    return true;
}

static void midi_close(serial_backend_t* self)
{
    winmm_state_t* st = (winmm_state_t*)self->state.midi.seq;
    if (!st) return;
    if (st->have_in) { midiInStop(st->in); midiInClose(st->in); }
    if (st->out) midiOutClose(st->out);
    free(st);
    self->state.midi.seq = NULL;
}

/* WinMM is message-oriented; re-assemble bytes into short messages here. */
static bool midi_send(serial_backend_t* self, uint8_t byte)
{
    winmm_state_t* st = (winmm_state_t*)self->state.midi.seq;
    if (!st) return false;
    /* Minimal running-status reassembly using the midi.port field as a small
     * state machine: port = (status<<16)|(have1<<8)|byte1. */
    static uint8_t status = 0, d1 = 0; static int need = 0, got = 0;
    if (byte & 0x80) {
        if (byte >= 0xF8) { midiOutShortMsg(st->out, byte); return true; } /* realtime */
        status = byte; got = 0;
        need = (byte >= 0xC0 && byte <= 0xDF) ? 1 : 2;
        return true;
    }
    if (!status) return true;
    if (got == 0) { d1 = byte; got = 1; if (need == 1) {
            midiOutShortMsg(st->out, (DWORD)(status | (d1 << 8))); got = 0; }
        return true; }
    midiOutShortMsg(st->out, (DWORD)(status | (d1 << 8) | (byte << 16)));
    got = 0;
    return true;
}

static bool midi_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.midi.rx_count <= 0) return false;
    *byte = self->state.midi.rx_buf[self->state.midi.rx_head];
    self->state.midi.rx_head =
        (self->state.midi.rx_head + 1) % (int)sizeof(self->state.midi.rx_buf);
    self->state.midi.rx_count--;
    return true;
}

static bool midi_poll(serial_backend_t* self) { return self->state.midi.rx_count > 0; }
static bool midi_connected(serial_backend_t* self) { return self->state.midi.seq != NULL; }

#endif /* MIDI_BACKEND_* */

#if defined(MIDI_BACKEND_ALSA) || defined(MIDI_BACKEND_COREMIDI) || defined(MIDI_BACKEND_WINMM)

serial_backend_t* serial_backend_midi_create(const char* target)
{
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;
    b->type = SERIAL_BACKEND_MIDI;
    b->open = midi_open;
    b->close = midi_close;
    b->send = midi_send;
    b->recv = midi_recv;
    b->poll = midi_poll;
    b->connected = midi_connected;
    if (target && target[0]) {
        strncpy(b->state.midi.target, target, sizeof(b->state.midi.target) - 1);
    }
    return b;
}

#else /* no real-time MIDI in this build — graceful stub */

serial_backend_t* serial_backend_midi_create(const char* target)
{
    (void)target;
    log_error("MIDI: this build has no real-time MIDI support (rebuild with MIDI=1)");
    return NULL;
}

#endif /* any MIDI_BACKEND_* */

/* ═══════════════════════════════════════════════════════════════════════
 *  Standard MIDI File backend — timed MIDI IN replay
 * ═══════════════════════════════════════════════════════════════════════ */

/* Monotonic clock in nanoseconds (real-time pacing of the song). */
static int64_t smf_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Push @p n bytes onto the SMF RX ring (dropping any overflow). */
static void smf_push(serial_backend_t* self, const uint8_t* b, int n)
{
    for (int i = 0; i < n; i++) {
        if (self->state.smf.rx_count >= (int)sizeof(self->state.smf.rx_buf)) break;
        self->state.smf.rx_buf[self->state.smf.rx_tail] = b[i];
        self->state.smf.rx_tail =
            (self->state.smf.rx_tail + 1) % (int)sizeof(self->state.smf.rx_buf);
        self->state.smf.rx_count++;
    }
}

/* Deliver every event whose scheduled time has elapsed since playback start. */
static void smf_pump(serial_backend_t* self)
{
    smf_t* song = (smf_t*)self->state.smf.song;
    if (!song || song->count == 0) return;

    if (self->state.smf.start_ns == 0) self->state.smf.start_ns = smf_now_ns();
    uint64_t elapsed_us =
        (uint64_t)(smf_now_ns() - self->state.smf.start_ns) / 1000ULL;

    while (self->state.smf.cursor < song->count) {
        smf_event_t* e = &song->events[self->state.smf.cursor];
        if (e->t_us > elapsed_us) break;
        smf_push(self, smf_event_bytes(song, self->state.smf.cursor), e->len);
        self->state.smf.cursor++;
    }

    if (self->state.smf.cursor >= song->count && self->state.smf.loop) {
        /* Restart: rebase the clock so the next loop begins now. */
        self->state.smf.cursor = 0;
        self->state.smf.start_ns = smf_now_ns();
    }
}

/* The song is parsed at create time, so open() just resets the play clock. */
static bool smf_open(serial_backend_t* self)
{
    self->state.smf.cursor = 0;
    self->state.smf.start_ns = 0;
    self->state.smf.rx_head = self->state.smf.rx_tail = self->state.smf.rx_count = 0;
    return self->state.smf.song != NULL;
}

static bool smf_recv(serial_backend_t* self, uint8_t* byte)
{
    smf_pump(self);
    if (self->state.smf.rx_count <= 0) return false;
    *byte = self->state.smf.rx_buf[self->state.smf.rx_head];
    self->state.smf.rx_head =
        (self->state.smf.rx_head + 1) % (int)sizeof(self->state.smf.rx_buf);
    self->state.smf.rx_count--;
    return true;
}

static bool smf_poll(serial_backend_t* self)
{
    smf_pump(self);
    return self->state.smf.rx_count > 0;
}

static bool smf_send(serial_backend_t* self, uint8_t byte)
{
    (void)self; (void)byte;   /* MIDI IN replay: the Oric's TX is discarded */
    return true;
}

static bool smf_connected(serial_backend_t* self)
{
    return self->state.smf.song != NULL;
}

static void smf_close(serial_backend_t* self)
{
    smf_t* song = (smf_t*)self->state.smf.song;
    if (song) { smf_free(song); free(song); self->state.smf.song = NULL; }
}

serial_backend_t* serial_backend_smf_create(const char* path, bool loop)
{
    if (!path || !path[0]) return NULL;
    smf_t* song = calloc(1, sizeof(smf_t));
    if (!song) return NULL;
    if (!smf_load(path, song)) {
        log_error("SMF: cannot parse MIDI file '%s'", path);
        free(song);
        return NULL;
    }
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) { smf_free(song); free(song); return NULL; }
    b->type = SERIAL_BACKEND_SMF;
    b->open = smf_open;             /* parsed at create; open() resets the clock */
    b->close = smf_close;
    b->send = smf_send;
    b->recv = smf_recv;
    b->poll = smf_poll;
    b->connected = smf_connected;
    b->state.smf.song = song;
    b->state.smf.cursor = 0;
    b->state.smf.start_ns = 0;      /* clock starts on first pump */
    b->state.smf.loop = loop;
    log_info("SMF: '%s' loaded — %zu events, %.1fs%s", path, song->count,
             song->total_us / 1.0e6, loop ? " (looping)" : "");
    return b;
}

void serial_backend_destroy(serial_backend_t* backend)
{
    if (!backend) return;
    if (backend->close) backend->close(backend);
    free(backend);
}
