/**
 * @file serial_picowifi.c
 * @brief PicoWiFiModemUSB (sodiumlb) backend for the ACIA 6551
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 *
 * Emulates the sodiumlb PicoWiFiModemUSB — a Raspberry Pi Pico W that
 * bridges USB CDC ↔ WiFi with a Hayes-style AT command set. On real
 * hardware the device plugs into LOCI over USB, and the LOCI firmware
 * exposes it to the Oric as an ACIA serial modem at $0380. From the Oric's
 * point of view it is "just an ACIA"; from the user's point of view it is a
 * WiFi modem driven by AT commands.
 *
 * What is emulated:
 *   - The v0.1.0 AT command set (see github.com/sodiumlb/PicoWiFiModemUSB):
 *     WiFi config (AT$SSID/AT$PASS/AT$MDNS), dialing (ATDT/ATDS/ATD),
 *     telnet (ATNET, AT$TTY/AT$TTS/AT$TTL), S-registers (S0/S2),
 *     result-code formatting (ATE/ATQ/ATV/ATX), flow control (AT&K/AT&D),
 *     speed dial (AT&Z), incoming server (AT$SP/AT&R), info (ATI),
 *     profile (AT&V/AT&W/AT&F), startup (AT$AE/AT$W), online (ATO),
 *     +++ escape (guard-timed, S2-configurable escape char), A/ repeat.
 *   - Data connections are real TCP sockets (telnet/raw).
 *
 * What is simulated:
 *   - WiFi association: AT$SSID/AT$PASS store credentials; the link is
 *     considered "up" once an SSID is set. There is no real radio.
 *   - ATGET/ATRD/ATRT and other internet-service commands return a
 *     plausible canned response — the emulator has no NIST clock.
 *
 * This is a separate module from serial_backend.c because the command set
 * is large; the picowifi_t state is heap-allocated and reached through the
 * opaque `state.picowifi.impl` pointer in serial_backend_t.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>

#include "io/serial_backend.h"
#include "utils/logging.h"

#define PW_FW_VERSION   "0.1.0"
#define PW_RX_BUFSZ     65536      /* 64KB receive ring (line → Oric) */
#define PW_DIAL_SLOTS   10         /* AT&Z0..9 speed-dial entries */
#define PW_DEFAULT_PORT 23         /* telnet */

/* ── Hayes result codes ── */
enum {
    PW_OK = 0,
    PW_CONNECT = 1,
    PW_RING = 2,
    PW_NO_CARRIER = 3,
    PW_ERROR = 4,
    PW_NO_DIALTONE = 6,
    PW_BUSY = 7,
};

typedef struct {
    /* ── Networking (TCP data connection) ── */
    int      sockfd;                /* data socket, -1 = idle */
    int      listen_fd;             /* incoming-server socket, -1 = none */
    char     host[256];             /* preset/last dial host */
    uint16_t port;                  /* preset/last dial port */
    uint8_t* rx_buf;                /* 64KB ring → Oric */
    int      rx_head, rx_tail, rx_count;

    /* ── AT command state machine ── */
    int      mode;                  /* 0 = command, 1 = data */
    char     cmd_buf[256];
    int      cmd_len;
    char     last_cmd[256];         /* for A/ repeat */

    /* ── +++ escape (guard-timed, S2 char) ── */
    int      plus_count;
    int64_t  silence;               /* poll-based silence counter */

    /* ── WiFi configuration / state ── */
    char     ssid[64];
    char     pass[64];
    char     mdns[64];              /* default "espmodem" */
    bool     wifi_up;               /* simulated association */

    /* ── Serial parameters (reported, not enforced) ── */
    int      baud;                  /* AT$SB, default 1200 */
    char     fmt[8];                /* AT$SU, default "8N1" */

    /* ── Telnet ── */
    int      telnet;                /* ATNET: 0=off, 1=real, 2=fake */
    char     tty_type[16];          /* AT$TTY, default "ansi" */
    int      tty_w, tty_h;          /* AT$TTS, default 80x24 */
    char     tty_loc[64];           /* AT$TTL */

    /* ── S-registers ── */
    int      s0_rings;              /* S0: auto-answer ring count */
    int      esc_char;              /* S2: escape char, default 43 ('+') */

    /* ── Result-code formatting ── */
    bool     echo;                  /* E: echo command chars */
    bool     quiet;                 /* Q: suppress result codes */
    bool     verbose;               /* V: 1=text, 0=numeric */
    int      xcode;                 /* X: 0=basic, 1=extended */

    /* ── Flow control / DTR ── */
    int      flow_k;                /* &K: 0=off, 1=RTS/CTS */
    int      dtr_d;                 /* &D: 0..3 */

    /* ── Speed dial 0..9 ── */
    struct {
        bool     used;
        char     host[256];
        uint16_t port;
        char     alias[32];
    } dial[PW_DIAL_SLOTS];

    /* ── Incoming server ── */
    uint16_t server_port;           /* $SP, 0 = disabled */
    char     server_pass[64];       /* &R */

    /* ── Startup ── */
    char     auto_exec[256];        /* $AE */
    int      startup_wait;          /* $W */
} picowifi_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  EINTR-safe socket I/O
 * ═══════════════════════════════════════════════════════════════════════ */

static ssize_t pw_read(int fd, void* buf, size_t n)
{
    ssize_t r;
    do { r = read(fd, buf, n); } while (r < 0 && errno == EINTR);
    return r;
}

static ssize_t pw_write(int fd, const void* buf, size_t n)
{
    ssize_t r;
    do { r = write(fd, buf, n); } while (r < 0 && errno == EINTR);
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  RX ring buffer (modem → Oric)
 * ═══════════════════════════════════════════════════════════════════════ */

static void pw_rx_push(picowifi_t* pw, uint8_t b)
{
    if (pw->rx_count >= PW_RX_BUFSZ) return;
    pw->rx_buf[pw->rx_head] = b;
    pw->rx_head = (pw->rx_head + 1) % PW_RX_BUFSZ;
    pw->rx_count++;
}

static bool pw_rx_pop(picowifi_t* pw, uint8_t* b)
{
    if (pw->rx_count <= 0) return false;
    *b = pw->rx_buf[pw->rx_tail];
    pw->rx_tail = (pw->rx_tail + 1) % PW_RX_BUFSZ;
    pw->rx_count--;
    return true;
}

static void pw_rx_str(picowifi_t* pw, const char* s)
{
    while (*s) pw_rx_push(pw, (uint8_t)*s++);
}

/* Emit a Hayes result code, honouring ATQ (quiet) and ATV (verbose). */
static void pw_result(picowifi_t* pw, int code)
{
    if (pw->quiet) return;
    if (pw->verbose) {
        const char* txt;
        switch (code) {
            case PW_OK:          txt = "OK";          break;
            case PW_CONNECT:     txt = "CONNECT";     break;
            case PW_RING:        txt = "RING";        break;
            case PW_NO_CARRIER:  txt = "NO CARRIER";  break;
            case PW_ERROR:       txt = "ERROR";       break;
            case PW_NO_DIALTONE: txt = "NO DIALTONE"; break;
            case PW_BUSY:        txt = "BUSY";        break;
            default:             txt = "ERROR";       break;
        }
        pw_rx_str(pw, "\r\n");
        pw_rx_str(pw, txt);
        pw_rx_str(pw, "\r\n");
    } else {
        char b[8];
        snprintf(b, sizeof(b), "%d\r", code);
        pw_rx_str(pw, b);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Defaults
 * ═══════════════════════════════════════════════════════════════════════ */

static void pw_set_defaults(picowifi_t* pw)
{
    /* Networking handles and credentials survive a soft ATZ reset on the
     * real device (they are stored), so only the session defaults reset. */
    pw->mode = 0;
    pw->cmd_len = 0;
    pw->plus_count = 0;
    pw->silence = 0;
    pw->echo = true;
    pw->quiet = false;
    pw->verbose = true;
    pw->xcode = 1;
    pw->s0_rings = 0;
    pw->esc_char = 43;          /* '+' */
    pw->telnet = 1;             /* real telnet by default */
    pw->flow_k = 0;
    pw->dtr_d = 0;
    if (pw->mdns[0] == '\0')
        snprintf(pw->mdns, sizeof(pw->mdns), "espmodem");
    if (pw->fmt[0] == '\0')
        snprintf(pw->fmt, sizeof(pw->fmt), "8N1");
    if (pw->baud == 0)
        pw->baud = 1200;
    if (pw->tty_type[0] == '\0')
        snprintf(pw->tty_type, sizeof(pw->tty_type), "ansi");
    if (pw->tty_w == 0) { pw->tty_w = 80; pw->tty_h = 24; }
}

/* Reset *everything* to factory state (AT&F). */
static void pw_factory_reset(picowifi_t* pw)
{
    pw->ssid[0] = pw->pass[0] = '\0';
    pw->mdns[0] = pw->fmt[0] = pw->tty_type[0] = pw->tty_loc[0] = '\0';
    pw->tty_w = pw->tty_h = 0;
    pw->baud = 0;
    pw->server_port = 0;
    pw->server_pass[0] = '\0';
    pw->auto_exec[0] = '\0';
    pw->startup_wait = 0;
    for (int i = 0; i < PW_DIAL_SLOTS; i++) pw->dial[i].used = false;
    pw_set_defaults(pw);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Dialing
 * ═══════════════════════════════════════════════════════════════════════ */

/* Open a non-blocking TCP connection. Returns fd or -1. */
static int pw_tcp_connect(const char* host, uint16_t port)
{
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%u", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0) {
        log_error("PicoWiFi: getaddrinfo(%s:%u): %s", host, port, gai_strerror(err));
        return -1;
    }
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* Bring the (simulated) WiFi link up if an SSID is configured. */
static bool pw_wifi_associate(picowifi_t* pw)
{
    if (pw->ssid[0] == '\0') return false;
    if (!pw->wifi_up) {
        pw->wifi_up = true;
        log_info("PicoWiFi: associated to SSID \"%s\" (simulated)", pw->ssid);
    }
    return true;
}

/* Dial host:port. Emits CONNECT/NO CARRIER and switches to data mode. */
static void pw_dial(picowifi_t* pw, const char* host, uint16_t port)
{
    if (!pw_wifi_associate(pw)) {
        log_warning("PicoWiFi: dial with no SSID configured");
        pw_result(pw, PW_NO_CARRIER);
        return;
    }
    int fd = pw_tcp_connect(host, port);
    if (fd < 0) {
        log_error("PicoWiFi: connect to %s:%u failed", host, port);
        pw_result(pw, PW_NO_CARRIER);
        return;
    }
    pw->sockfd = fd;
    pw->mode = 1;
    pw->plus_count = 0;
    strncpy(pw->host, host, sizeof(pw->host) - 1);
    pw->host[sizeof(pw->host) - 1] = '\0';
    pw->port = port;
    log_info("PicoWiFi: CONNECT %s:%u (fd=%d)", host, port, fd);
    pw_result(pw, PW_CONNECT);
}

/* Parse "host[:port]" into host/port (default telnet port). */
static void pw_parse_hostport(const char* s, char* host, size_t hsz, uint16_t* port)
{
    *port = PW_DEFAULT_PORT;
    host[0] = '\0';
    while (*s == ' ') s++;
    const char* colon = strrchr(s, ':');
    if (colon) {
        size_t hl = (size_t)(colon - s);
        if (hl >= hsz) hl = hsz - 1;
        memcpy(host, s, hl);
        host[hl] = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host, s, hsz - 1);
        host[hsz - 1] = '\0';
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  AT command parser
 * ═══════════════════════════════════════════════════════════════════════ */

/* Case-insensitive prefix match; returns the tail after the prefix, or NULL. */
static const char* pw_match(const char* s, const char* prefix)
{
    size_t n = strlen(prefix);
    if (strncasecmp(s, prefix, n) == 0) return s + n;
    return NULL;
}

static void pw_emit_str_line(picowifi_t* pw, const char* s)
{
    pw_rx_str(pw, "\r\n");
    pw_rx_str(pw, s);
    pw_rx_str(pw, "\r\n");
}

static void pw_process_at(picowifi_t* pw, const char* line)
{
    /* "A/" repeats the last command (handled before AT stripping). */
    if ((line[0] == 'A' || line[0] == 'a') && line[1] == '/') {
        line = pw->last_cmd;
    } else {
        strncpy(pw->last_cmd, line, sizeof(pw->last_cmd) - 1);
        pw->last_cmd[sizeof(pw->last_cmd) - 1] = '\0';
    }

    /* Strip leading "AT" (the prefix is mandatory on real hardware). */
    if ((line[0] == 'A' || line[0] == 'a') &&
        (line[1] == 'T' || line[1] == 't')) {
        line += 2;
    } else if (line[0] != '\0') {
        pw_result(pw, PW_ERROR);
        return;
    }

    /* Bare "AT" → OK */
    if (line[0] == '\0') { pw_result(pw, PW_OK); return; }

    const char* a;

    /* ── WiFi configuration ── */
    if ((a = pw_match(line, "$SSID"))) {
        if (*a == '?') { pw_emit_str_line(pw, pw->ssid); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->ssid, a + 1, sizeof(pw->ssid) - 1);
            pw->ssid[sizeof(pw->ssid) - 1] = '\0';
            pw->wifi_up = false;  /* re-associate on next dial */
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "$PASS"))) {
        if (*a == '?') { pw_emit_str_line(pw, "********"); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->pass, a + 1, sizeof(pw->pass) - 1);
            pw->pass[sizeof(pw->pass) - 1] = '\0';
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "$MDNS"))) {
        if (*a == '?') { pw_emit_str_line(pw, pw->mdns); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->mdns, a + 1, sizeof(pw->mdns) - 1);
            pw->mdns[sizeof(pw->mdns) - 1] = '\0';
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Serial parameters ── */
    if ((a = pw_match(line, "$SB"))) {
        if (*a == '?') {
            char b[16]; snprintf(b, sizeof(b), "%d", pw->baud);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a == '=') { pw->baud = atoi(a + 1); pw_result(pw, PW_OK); }
        else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "$SU"))) {
        if (*a == '?') { pw_emit_str_line(pw, pw->fmt); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->fmt, a + 1, sizeof(pw->fmt) - 1);
            pw->fmt[sizeof(pw->fmt) - 1] = '\0';
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Telnet terminal ── */
    if ((a = pw_match(line, "$TTY"))) {
        if (*a == '?') { pw_emit_str_line(pw, pw->tty_type); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->tty_type, a + 1, sizeof(pw->tty_type) - 1);
            pw->tty_type[sizeof(pw->tty_type) - 1] = '\0';
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "$TTS"))) {
        if (*a == '?') {
            char b[16]; snprintf(b, sizeof(b), "%dx%d", pw->tty_w, pw->tty_h);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a == '=') {
            int w = 0, h = 0;
            if (sscanf(a + 1, "%dx%d", &w, &h) == 2) {
                pw->tty_w = w; pw->tty_h = h; pw_result(pw, PW_OK);
            } else pw_result(pw, PW_ERROR);
        } else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "$TTL"))) {
        if (*a == '?') { pw_emit_str_line(pw, pw->tty_loc); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->tty_loc, a + 1, sizeof(pw->tty_loc) - 1);
            pw->tty_loc[sizeof(pw->tty_loc) - 1] = '\0';
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Incoming server ── */
    if ((a = pw_match(line, "$SP"))) {
        if (*a == '?') {
            char b[16]; snprintf(b, sizeof(b), "%u", pw->server_port);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a == '=') { pw->server_port = (uint16_t)atoi(a + 1); pw_result(pw, PW_OK); }
        else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "$AE"))) {
        if (*a == '?') { pw_emit_str_line(pw, pw->auto_exec); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->auto_exec, a + 1, sizeof(pw->auto_exec) - 1);
            pw->auto_exec[sizeof(pw->auto_exec) - 1] = '\0';
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "$W"))) {
        if (*a == '?') {
            char b[16]; snprintf(b, sizeof(b), "%d", pw->startup_wait);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a == '=') { pw->startup_wait = atoi(a + 1); pw_result(pw, PW_OK); }
        else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Dialing ── */
    if ((a = pw_match(line, "DT")) || (a = pw_match(line, "DP"))) {
        /* ATDT[+=-]host[:port] — leading +/=/- variants are ignored. */
        while (*a == '+' || *a == '=' || *a == '-' || *a == ' ') a++;
        char host[256]; uint16_t port;
        pw_parse_hostport(a, host, sizeof(host), &port);
        if (host[0] == '\0' && pw->host[0] != '\0') { /* redial preset */
            strncpy(host, pw->host, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            port = pw->port ? pw->port : PW_DEFAULT_PORT;
        }
        pw_dial(pw, host, port);
        return;
    }
    if ((a = pw_match(line, "DS"))) {
        int n = atoi(a);
        if (n >= 0 && n < PW_DIAL_SLOTS && pw->dial[n].used) {
            pw_dial(pw, pw->dial[n].host, pw->dial[n].port);
        } else {
            pw_result(pw, PW_NO_CARRIER);
        }
        return;
    }
    if ((a = pw_match(line, "D"))) {
        char host[256]; uint16_t port;
        pw_parse_hostport(a, host, sizeof(host), &port);
        if (host[0] == '\0' && pw->host[0] != '\0') {
            strncpy(host, pw->host, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            port = pw->port ? pw->port : PW_DEFAULT_PORT;
        }
        pw_dial(pw, host, port);
        return;
    }

    /* ── WiFi connection control ── */
    if ((a = pw_match(line, "C"))) {
        if (*a == '?') {
            pw_emit_str_line(pw, pw->wifi_up ? "CONNECTED" : "DISCONNECTED");
            pw_result(pw, PW_OK);
        } else if (*a == '1') {
            if (pw_wifi_associate(pw)) pw_result(pw, PW_OK);
            else pw_result(pw, PW_ERROR);
        } else if (*a == '0') {
            pw->wifi_up = false;
            log_info("PicoWiFi: WiFi disconnected (simulated)");
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Telnet enable ── */
    if ((a = pw_match(line, "NET"))) {
        if (*a == '?') {
            char b[4]; snprintf(b, sizeof(b), "%d", pw->telnet);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a >= '0' && *a <= '2') {
            pw->telnet = *a - '0'; pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Speed dial AT&Zn=host[:port],alias ── */
    if ((a = pw_match(line, "&Z"))) {
        int n = *a - '0';
        if (n < 0 || n >= PW_DIAL_SLOTS) { pw_result(pw, PW_ERROR); return; }
        a++;
        if (*a == '?') {
            if (pw->dial[n].used) {
                char b[300];
                snprintf(b, sizeof(b), "%s:%u,%s",
                         pw->dial[n].host, pw->dial[n].port, pw->dial[n].alias);
                pw_emit_str_line(pw, b);
            } else pw_emit_str_line(pw, "");
            pw_result(pw, PW_OK);
        } else if (*a == '=') {
            a++;
            char hp[256] = {0}, alias[32] = {0};
            const char* comma = strchr(a, ',');
            if (comma) {
                size_t hl = (size_t)(comma - a);
                if (hl >= sizeof(hp)) hl = sizeof(hp) - 1;
                memcpy(hp, a, hl); hp[hl] = '\0';
                strncpy(alias, comma + 1, sizeof(alias) - 1);
            } else {
                strncpy(hp, a, sizeof(hp) - 1);
            }
            char host[256]; uint16_t port;
            pw_parse_hostport(hp, host, sizeof(host), &port);
            snprintf(pw->dial[n].host, sizeof(pw->dial[n].host), "%s", host);
            pw->dial[n].port = port;
            snprintf(pw->dial[n].alias, sizeof(pw->dial[n].alias), "%s", alias);
            pw->dial[n].used = true;
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Incoming password AT&R ── */
    if ((a = pw_match(line, "&R"))) {
        if (*a == '?') { pw_emit_str_line(pw, "********"); pw_result(pw, PW_OK); }
        else if (*a == '=') {
            strncpy(pw->server_pass, a + 1, sizeof(pw->server_pass) - 1);
            pw->server_pass[sizeof(pw->server_pass) - 1] = '\0';
            pw_result(pw, PW_OK);
        } else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Flow control / DTR ── */
    if ((a = pw_match(line, "&K"))) {
        if (*a == '?') {
            char b[4]; snprintf(b, sizeof(b), "%d", pw->flow_k);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a >= '0' && *a <= '1') { pw->flow_k = *a - '0'; pw_result(pw, PW_OK); }
        else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "&D"))) {
        if (*a == '?') {
            char b[4]; snprintf(b, sizeof(b), "%d", pw->dtr_d);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a >= '0' && *a <= '3') { pw->dtr_d = *a - '0'; pw_result(pw, PW_OK); }
        else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Profile ── */
    if ((a = pw_match(line, "&V"))) {
        char b[256];
        snprintf(b, sizeof(b), "E%d Q%d V%d X%d &K%d &D%d S0=%d S2=%d NET%d",
                 pw->echo, pw->quiet, pw->verbose, pw->xcode,
                 pw->flow_k, pw->dtr_d, pw->s0_rings, pw->esc_char, pw->telnet);
        pw_emit_str_line(pw, b);
        snprintf(b, sizeof(b), "SSID:%s MDNS:%s BAUD:%d FMT:%s TTY:%s %dx%d",
                 pw->ssid, pw->mdns, pw->baud, pw->fmt,
                 pw->tty_type, pw->tty_w, pw->tty_h);
        pw_emit_str_line(pw, b);
        pw_result(pw, PW_OK);
        return;
    }
    if (pw_match(line, "&W")) { pw_result(pw, PW_OK); return; }  /* save: no-op */
    if (pw_match(line, "&F")) { pw_factory_reset(pw); pw_result(pw, PW_OK); return; }

    /* ── S-registers ── */
    if ((a = pw_match(line, "S0"))) {
        if (*a == '?') {
            char b[8]; snprintf(b, sizeof(b), "%d", pw->s0_rings);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a == '=') { pw->s0_rings = atoi(a + 1); pw_result(pw, PW_OK); }
        else pw_result(pw, PW_ERROR);
        return;
    }
    if ((a = pw_match(line, "S2"))) {
        if (*a == '?') {
            char b[8]; snprintf(b, sizeof(b), "%d", pw->esc_char);
            pw_emit_str_line(pw, b); pw_result(pw, PW_OK);
        } else if (*a == '=') { pw->esc_char = atoi(a + 1); pw_result(pw, PW_OK); }
        else pw_result(pw, PW_ERROR);
        return;
    }

    /* ── Result-code formatting ── */
    if ((a = pw_match(line, "E"))) {
        pw->echo = (*a != '0');
        pw_result(pw, PW_OK);
        return;
    }
    if ((a = pw_match(line, "Q"))) {
        pw->quiet = (*a == '1');
        pw_result(pw, PW_OK);
        return;
    }
    if ((a = pw_match(line, "V"))) {
        pw->verbose = (*a != '0');
        pw_result(pw, PW_OK);
        return;
    }
    if ((a = pw_match(line, "X"))) {
        pw->xcode = (*a == '0') ? 0 : 1;
        pw_result(pw, PW_OK);
        return;
    }

    /* ── Hangup / answer / online ── */
    if (pw_match(line, "H")) {
        if (pw->sockfd >= 0) {
            close(pw->sockfd);
            pw->sockfd = -1;
            log_info("PicoWiFi: hangup");
        }
        pw->mode = 0;
        pw_result(pw, PW_OK);
        return;
    }
    if (pw_match(line, "O")) {
        if (pw->sockfd >= 0) { pw->mode = 1; pw_result(pw, PW_CONNECT); }
        else pw_result(pw, PW_NO_CARRIER);
        return;
    }
    if (pw_match(line, "A")) {
        /* ATA: answer an incoming connection (none in this emulation). */
        pw_result(pw, PW_NO_CARRIER);
        return;
    }

    /* ── Info ── */
    if (pw_match(line, "I")) {
        char b[160];
        pw_emit_str_line(pw, "PicoWiFiModemUSB (emulated)");
        snprintf(b, sizeof(b), "Firmware v%s", PW_FW_VERSION);
        pw_emit_str_line(pw, b);
        snprintf(b, sizeof(b), "WiFi: %s %s",
                 pw->ssid[0] ? pw->ssid : "(not configured)",
                 pw->wifi_up ? "UP" : "DOWN");
        pw_emit_str_line(pw, b);
        pw_emit_str_line(pw, "IP: 192.168.0.42 (simulated)");
        pw_result(pw, PW_OK);
        return;
    }

    /* ── Time / HTTP (simulated, no real internet service) ── */
    if (pw_match(line, "RD") || pw_match(line, "RT")) {
        pw_emit_str_line(pw, "1970-01-01 00:00:00 UTC (no clock)");
        pw_result(pw, PW_OK);
        return;
    }
    if (pw_match(line, "GET")) {
        pw_emit_str_line(pw, "(ATGET not supported in emulation)");
        pw_result(pw, PW_OK);
        return;
    }

    /* ── Reset ── */
    if (pw_match(line, "Z")) {
        pw_set_defaults(pw);
        pw_result(pw, PW_OK);
        return;
    }

    /* Unknown */
    pw_result(pw, PW_ERROR);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Backend vtable
 * ═══════════════════════════════════════════════════════════════════════ */

static bool picowifi_open(serial_backend_t* self)
{
    picowifi_t* pw = (picowifi_t*)self->state.picowifi.impl;
    pw->rx_buf = malloc(PW_RX_BUFSZ);
    if (!pw->rx_buf) {
        log_error("PicoWiFi: failed to allocate RX buffer");
        return false;
    }
    pw->rx_head = pw->rx_tail = pw->rx_count = 0;
    pw->sockfd = -1;
    pw->listen_fd = -1;
    pw_set_defaults(pw);
    log_info("PicoWiFi: backend opened (SSID:%s)",
             pw->ssid[0] ? pw->ssid : "(unset)");
    return true;
}

static void picowifi_close(serial_backend_t* self)
{
    picowifi_t* pw = (picowifi_t*)self->state.picowifi.impl;
    if (!pw) return;
    if (pw->sockfd >= 0) { close(pw->sockfd); pw->sockfd = -1; }
    if (pw->listen_fd >= 0) { close(pw->listen_fd); pw->listen_fd = -1; }
    free(pw->rx_buf);
    pw->rx_buf = NULL;
    free(pw);
    self->state.picowifi.impl = NULL;
    log_info("PicoWiFi: backend closed");
}

static bool picowifi_send(serial_backend_t* self, uint8_t byte)
{
    picowifi_t* pw = (picowifi_t*)self->state.picowifi.impl;

    /* ── Data mode ── */
    if (pw->mode == 1) {
        /* +++ escape with guard time (escape char is S2-configurable). */
        if ((int)byte == pw->esc_char && pw->silence >= 50) {
            pw->plus_count++;
            if (pw->plus_count >= 3) {
                pw->mode = 0;
                pw->plus_count = 0;
                pw->cmd_len = 0;
                pw->silence = 0;
                log_info("PicoWiFi: +++ escape → command mode");
                pw_result(pw, PW_OK);
                return true;
            }
            pw->silence = 0;
            return true;
        }
        /* Flush any pending escape chars that turned out to be data. */
        if (pw->sockfd >= 0 && pw->plus_count > 0) {
            uint8_t e = (uint8_t)pw->esc_char;
            for (int i = 0; i < pw->plus_count && i < 3; i++)
                (void)pw_write(pw->sockfd, &e, 1);
        }
        pw->plus_count = 0;
        pw->silence = 0;

        if (pw->sockfd >= 0) {
            ssize_t n = pw_write(pw->sockfd, &byte, 1);
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                log_info("PicoWiFi: write error, carrier lost");
                close(pw->sockfd);
                pw->sockfd = -1;
                pw->mode = 0;
                pw_result(pw, PW_NO_CARRIER);
                return false;
            }
        }
        return true;
    }

    /* ── Command mode: accumulate AT line ── */
    if (pw->echo) pw_rx_push(pw, byte);

    if (byte == '\r' || byte == '\n') {
        if (pw->cmd_len > 0) {
            pw->cmd_buf[pw->cmd_len] = '\0';
            pw_process_at(pw, pw->cmd_buf);
            pw->cmd_len = 0;
        }
    } else if (byte == '\b' || byte == 127) {
        if (pw->cmd_len > 0) pw->cmd_len--;
    } else {
        if (pw->cmd_len < (int)sizeof(pw->cmd_buf) - 1)
            pw->cmd_buf[pw->cmd_len++] = (char)byte;
    }
    return true;
}

static bool picowifi_recv(serial_backend_t* self, uint8_t* byte)
{
    picowifi_t* pw = (picowifi_t*)self->state.picowifi.impl;
    pw->silence++;

    if (pw->mode == 1 && pw->sockfd >= 0) {
        uint8_t tmp[256];
        ssize_t n = pw_read(pw->sockfd, tmp, sizeof(tmp));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) pw_rx_push(pw, tmp[i]);
        } else if (n == 0) {
            log_info("PicoWiFi: peer closed connection");
            close(pw->sockfd);
            pw->sockfd = -1;
            pw->mode = 0;
            pw_result(pw, PW_NO_CARRIER);
        }
    }
    return pw_rx_pop(pw, byte);
}

static bool picowifi_poll(serial_backend_t* self)
{
    picowifi_t* pw = (picowifi_t*)self->state.picowifi.impl;
    if (pw->rx_count > 0) return true;
    if (pw->mode == 1 && pw->sockfd >= 0) {
        struct pollfd pfd = { .fd = pw->sockfd, .events = POLLIN };
        return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
    }
    return false;
}

static bool picowifi_connected(serial_backend_t* self)
{
    picowifi_t* pw = (picowifi_t*)self->state.picowifi.impl;
    return pw->mode == 1 && pw->sockfd >= 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Factory
 * ═══════════════════════════════════════════════════════════════════════ */

serial_backend_t* serial_backend_picowifi_create(const char* ssid, const char* pass)
{
    serial_backend_t* b = calloc(1, sizeof(serial_backend_t));
    if (!b) return NULL;

    picowifi_t* pw = calloc(1, sizeof(picowifi_t));
    if (!pw) { free(b); return NULL; }

    if (ssid) { strncpy(pw->ssid, ssid, sizeof(pw->ssid) - 1); }
    if (pass) { strncpy(pw->pass, pass, sizeof(pw->pass) - 1); }

    b->type = SERIAL_BACKEND_PICOWIFI;
    b->open = picowifi_open;
    b->close = picowifi_close;
    b->send = picowifi_send;
    b->recv = picowifi_recv;
    b->poll = picowifi_poll;
    b->connected = picowifi_connected;
    b->state.picowifi.impl = pw;
    return b;
}
