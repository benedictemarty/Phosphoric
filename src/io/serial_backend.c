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

void serial_backend_destroy(serial_backend_t* backend)
{
    if (!backend) return;
    if (backend->close) backend->close(backend);
    free(backend);
}
