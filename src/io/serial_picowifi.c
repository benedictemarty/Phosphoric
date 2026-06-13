/**
 * @file serial_picowifi.c
 * @brief PicoWiFiModemUSB (sodiumlb) backend for the ACIA 6551
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 *
 * Emulates the sodiumlb PicoWiFiModemUSB — a Raspberry Pi Pico W that
 * bridges USB CDC ↔ WiFi with a Hayes-style AT command set. On real
 * hardware the device plugs into LOCI over USB, and the LOCI firmware
 * exposes it to the Oric as an ACIA serial modem at $0380.
 *
 * Sprint 45 delivered the AT command surface. Sprint 46 aligns behaviour
 * with the real firmware source (clone sodiumlb/PicoWiFiModemUSB):
 *   - exact factory defaults (mDNS "picomodem", 9600 baud, location
 *     "Computer Room", 3 predefined speed dials)
 *   - faithful result-code enum (R_NO_ANSWER=5, R_RING_IP=6)
 *   - compound command lines ("ATS0=1 X1 Q0"): a token loop where only the
 *     last token emits a result, an unknown token aborts with ERROR
 *   - ATC? numeric (0/1), ATX extended codes (CONNECT <speed>,
 *     NO CARRIER (HH:MM:SS)), ATZ as a no-op (the real device cannot reset
 *     without dropping USB)
 *
 * Deferred to later sprints (see ROADMAP):
 *   - 47: telnet protocol (CR+NUL, IAC negotiation), real +++ guard time,
 *         ATDT +/=/- prefixes, AT$AYT
 *   - 48: ATGET (HTTP), ATRD/ATRT (NIST), alias/7-digit dial, AT$BM
 *   - 49: NVRAM persistence (AT&W/&V0/&V1), boot auto-reconnect
 *
 * WiFi association is simulated; data connections are real TCP sockets.
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
#include <time.h>
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
#define PW_DEFAULT_BAUD 9600       /* firmware DEFAULT_SPEED */

/* Hayes result codes — exact enum from firmware types.h */
enum {
    PW_OK = 0,
    PW_CONNECT = 1,
    PW_RING = 2,
    PW_NO_CARRIER = 3,
    PW_ERROR = 4,
    PW_NO_ANSWER = 5,
    PW_RING_IP = 6,
};

typedef struct {
    /* ── Networking (TCP data connection) ── */
    int      sockfd;                /* data socket, -1 = idle */
    int      listen_fd;             /* incoming-server socket, -1 = none */
    char     host[256];             /* preset/last dial host */
    uint16_t port;                  /* preset/last dial port */
    uint8_t* rx_buf;                /* 64KB ring → Oric */
    int      rx_head, rx_tail, rx_count;
    time_t   connect_time;          /* set on dial, used for NO CARRIER (X1) */

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
    char     mdns[64];              /* default "picomodem" */
    bool     wifi_up;               /* simulated association */

    /* ── Serial parameters (reported, not enforced) ── */
    int      baud;                  /* AT$SB, default 9600 */
    char     fmt[8];                /* AT$SU, default "8N1" */

    /* ── Telnet ── */
    int      telnet;                /* ATNET: 0=off, 1=real, 2=fake */
    char     tty_type[16];          /* AT$TTY, default "ansi" */
    int      tty_w, tty_h;          /* AT$TTS, default 80x24 */
    char     tty_loc[64];           /* AT$TTL, default "Computer Room" */

    /* ── S-registers ── */
    int      s0_rings;              /* S0: auto-answer ring count */
    int      esc_char;              /* S2: escape char, default 43 ('+') */

    /* ── Result-code formatting ── */
    bool     echo;                  /* E: echo command chars */
    bool     quiet;                 /* Q: suppress result codes */
    bool     verbose;               /* V: 1=text, 0=numeric */
    int      xcode;                 /* X: 0=basic, 1=extended */

    /* ── Flow control / DTR (stored, inert on USB-CDC variant) ── */
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

/* A query value is printed on its own CRLF-delimited line. */
static void pw_qval(picowifi_t* pw, const char* s)
{
    pw_rx_str(pw, "\r\n");
    pw_rx_str(pw, s);
    pw_rx_str(pw, "\r\n");
}

/* Emit a Hayes result code, honouring ATQ (quiet), ATV (verbose), ATX
 * (extended). Mirrors firmware sendResult(). */
static void pw_result(picowifi_t* pw, int code)
{
    if (pw->quiet) return;

    if (!pw->verbose) {
        int c = (code == PW_RING_IP) ? PW_RING : code;
        char b[12];
        snprintf(b, sizeof(b), "%d", c);
        pw_rx_str(pw, "\r\n");
        pw_rx_str(pw, b);
        pw_rx_str(pw, "\r\n");
        return;
    }

    pw_rx_str(pw, "\r\n");
    switch (code) {
        case PW_CONNECT:
            pw_rx_str(pw, "CONNECT");
            if (pw->xcode) {
                char b[16];
                snprintf(b, sizeof(b), " %d", pw->baud);
                pw_rx_str(pw, b);
            }
            break;
        case PW_NO_CARRIER:
            pw_rx_str(pw, "NO CARRIER");
            if (pw->xcode && pw->connect_time) {
                long s = (long)(time(NULL) - pw->connect_time);
                if (s < 0) s = 0;
                char b[48];
                snprintf(b, sizeof(b), " (%02ld:%02ld:%02ld)",
                         s / 3600, (s / 60) % 60, s % 60);
                pw_rx_str(pw, b);
            }
            break;
        case PW_RING:
        case PW_RING_IP:   pw_rx_str(pw, "RING");      break;
        case PW_NO_ANSWER: pw_rx_str(pw, "NO ANSWER"); break;
        case PW_ERROR:     pw_rx_str(pw, "ERROR");     break;
        case PW_OK:
        default:           pw_rx_str(pw, "OK");        break;
    }
    pw_rx_str(pw, "\r\n");
}

/* Emit OK only when the command line is fully consumed (compound lines:
 * intermediate tokens are silent). Returns the pointer unchanged. */
static const char* pw_end(picowifi_t* pw, const char* p)
{
    if (!*p) pw_result(pw, PW_OK);
    return p;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Factory defaults (firmware factoryDefaults)
 * ═══════════════════════════════════════════════════════════════════════ */

static void pw_factory(picowifi_t* pw, bool keep_creds)
{
    char ssid_save[64], pass_save[64];
    snprintf(ssid_save, sizeof(ssid_save), "%s", pw->ssid);
    snprintf(pass_save, sizeof(pass_save), "%s", pw->pass);

    pw->ssid[0] = pw->pass[0] = '\0';
    snprintf(pw->mdns, sizeof(pw->mdns), "picomodem");
    pw->baud = PW_DEFAULT_BAUD;
    snprintf(pw->fmt, sizeof(pw->fmt), "8N1");
    pw->telnet = 1;                 /* REAL_TELNET */
    snprintf(pw->tty_type, sizeof(pw->tty_type), "ansi");
    pw->tty_w = 80; pw->tty_h = 24;
    snprintf(pw->tty_loc, sizeof(pw->tty_loc), "Computer Room");
    pw->s0_rings = 0;
    pw->esc_char = 43;              /* '+' */
    pw->echo = true;
    pw->quiet = false;
    pw->verbose = true;            /* V1 */
    pw->xcode = 1;                 /* X1 */
    pw->flow_k = 0;
    pw->dtr_d = 0;
    pw->server_port = 0;
    pw->server_pass[0] = '\0';
    pw->auto_exec[0] = '\0';
    pw->startup_wait = 0;

    /* 3 predefined speed dials (firmware factoryDefaults). The real device
     * stores a leading '+' on each (forces FAKE_TELNET) — telnet handling
     * is sprint 47, so the bare host:port is stored for now. */
    for (int i = 0; i < PW_DIAL_SLOTS; i++) pw->dial[i].used = false;
    pw->dial[0].used = true;
    snprintf(pw->dial[0].host, sizeof(pw->dial[0].host), "particlesbbs.dyndns.org");
    pw->dial[0].port = 6400;
    snprintf(pw->dial[0].alias, sizeof(pw->dial[0].alias), "particles");
    pw->dial[1].used = true;
    snprintf(pw->dial[1].host, sizeof(pw->dial[1].host), "altair.virtualaltair.com");
    pw->dial[1].port = 4667;
    snprintf(pw->dial[1].alias, sizeof(pw->dial[1].alias), "altair");
    pw->dial[2].used = true;
    snprintf(pw->dial[2].host, sizeof(pw->dial[2].host), "heatwave.ddns.net");
    pw->dial[2].port = 9640;
    snprintf(pw->dial[2].alias, sizeof(pw->dial[2].alias), "heatwave");

    if (keep_creds) {
        snprintf(pw->ssid, sizeof(pw->ssid), "%s", ssid_save);
        snprintf(pw->pass, sizeof(pw->pass), "%s", pass_save);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Dialing
 * ═══════════════════════════════════════════════════════════════════════ */

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
    pw->connect_time = time(NULL);
    snprintf(pw->host, sizeof(pw->host), "%s", host);
    pw->port = port;
    log_info("PicoWiFi: CONNECT %s:%u (fd=%d)", host, port, fd);
    pw_result(pw, PW_CONNECT);
}

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
        snprintf(host, hsz, "%s", s);
    }
}

/* Dial from a raw argument (consumes the rest of the line). */
static void pw_do_dial(picowifi_t* pw, const char* arg)
{
    /* Telnet prefixes +/=/- are skipped for now (sprint 47 will act on
     * them to force the session telnet mode). */
    while (*arg == '+' || *arg == '=' || *arg == '-' || *arg == ' ') arg++;
    char host[256]; uint16_t port;
    pw_parse_hostport(arg, host, sizeof(host), &port);
    if (host[0] == '\0' && pw->host[0] != '\0') {  /* redial preset */
        snprintf(host, sizeof(host), "%s", pw->host);
        port = pw->port ? pw->port : PW_DEFAULT_PORT;
    }
    pw_dial(pw, host, port);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  AT command parsing
 * ═══════════════════════════════════════════════════════════════════════ */

static const char* pw_match(const char* s, const char* prefix)
{
    size_t n = strlen(prefix);
    if (strncasecmp(s, prefix, n) == 0) return s + n;
    return NULL;
}

static const char* pw_skip_digits(const char* p)
{
    while (isdigit((unsigned char)*p)) p++;
    return p;
}

/* Copy the rest of the line as a string value; returns end-of-line. */
static const char* pw_setstr(char* dst, size_t dsz, const char* a)
{
    snprintf(dst, dsz, "%s", a);
    return a + strlen(a);
}

/* Dispatch one token. Returns the pointer just past the consumed token
 * (config commands emit OK only if at end; dial/info emit their own
 * result), or NULL if the token is unknown. */
static const char* pw_dispatch_one(picowifi_t* pw, const char* p)
{
    const char* a;

    /* AT? — quick help */
    if (*p == '?') {
        pw_qval(pw, "PicoWiFiModemUSB AT set: $SSID $PASS C D NET S0 S2 E Q V X &Z &V I");
        return pw_end(pw, p + 1);
    }

    /* ── $ proprietary commands ── (tested before single letters) */
    if ((a = pw_match(p, "$SSID"))) {
        if (*a == '?') { pw_qval(pw, pw->ssid); return pw_end(pw, a + 1); }
        if (*a == '=') { pw->wifi_up = false; return pw_end(pw, pw_setstr(pw->ssid, sizeof(pw->ssid), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$PASS"))) {
        /* Firmware shows the actual value on query (no masking). */
        if (*a == '?') { pw_qval(pw, pw->pass); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->pass, sizeof(pw->pass), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$MDNS"))) {
        if (*a == '?') { pw_qval(pw, pw->mdns); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->mdns, sizeof(pw->mdns), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$SB"))) {
        if (*a == '?') { char b[16]; snprintf(b, sizeof(b), "%d", pw->baud); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a == '=') { pw->baud = atoi(a + 1); return pw_end(pw, pw_skip_digits(a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$SU"))) {
        if (*a == '?') { pw_qval(pw, pw->fmt); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->fmt, sizeof(pw->fmt), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$TTY"))) {
        if (*a == '?') { pw_qval(pw, pw->tty_type); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->tty_type, sizeof(pw->tty_type), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$TTS"))) {
        if (*a == '?') {
            char b[16]; snprintf(b, sizeof(b), "%dx%d", pw->tty_w, pw->tty_h);
            pw_qval(pw, b); return pw_end(pw, a + 1);
        }
        if (*a == '=') {
            int w = 0, h = 0;
            if (sscanf(a + 1, "%dx%d", &w, &h) == 2) { pw->tty_w = w; pw->tty_h = h; }
            return pw_end(pw, a + strlen(a));
        }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$TTL"))) {
        if (*a == '?') { pw_qval(pw, pw->tty_loc); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->tty_loc, sizeof(pw->tty_loc), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$SP"))) {
        if (*a == '?') { char b[16]; snprintf(b, sizeof(b), "%u", pw->server_port); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a == '=') { pw->server_port = (uint16_t)atoi(a + 1); return pw_end(pw, pw_skip_digits(a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$AE"))) {
        if (*a == '?') { pw_qval(pw, pw->auto_exec); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->auto_exec, sizeof(pw->auto_exec), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "$W"))) {
        if (*a == '?') { char b[8]; snprintf(b, sizeof(b), "%d", pw->startup_wait); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a == '=') { pw->startup_wait = atoi(a + 1); return pw_end(pw, pw_skip_digits(a + 1)); }
        return pw_end(pw, a);
    }

    /* ── & extended commands ── */
    if ((a = pw_match(p, "&Z"))) {
        int n = (*a >= '0' && *a <= '9') ? (*a - '0') : -1;
        if (n < 0) return pw_end(pw, a);
        a++;
        if (*a == '?') {
            if (pw->dial[n].used) {
                char b[300];
                snprintf(b, sizeof(b), "%s:%u,%s",
                         pw->dial[n].host, pw->dial[n].port, pw->dial[n].alias);
                pw_qval(pw, b);
            } else pw_qval(pw, "");
            return pw_end(pw, a + 1);
        }
        if (*a == '=') {
            a++;
            char hp[256] = {0}, alias[32] = {0};
            const char* comma = strchr(a, ',');
            if (comma) {
                size_t hl = (size_t)(comma - a);
                if (hl >= sizeof(hp)) hl = sizeof(hp) - 1;
                memcpy(hp, a, hl); hp[hl] = '\0';
                snprintf(alias, sizeof(alias), "%s", comma + 1);
            } else {
                snprintf(hp, sizeof(hp), "%s", a);
            }
            char host[256]; uint16_t port;
            pw_parse_hostport(hp, host, sizeof(host), &port);
            snprintf(pw->dial[n].host, sizeof(pw->dial[n].host), "%s", host);
            pw->dial[n].port = port;
            snprintf(pw->dial[n].alias, sizeof(pw->dial[n].alias), "%s", alias);
            pw->dial[n].used = true;
            return pw_end(pw, a + strlen(a));
        }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "&R"))) {
        if (*a == '?') { pw_qval(pw, pw->server_pass); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->server_pass, sizeof(pw->server_pass), a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "&K"))) {
        if (*a == '?') { char b[4]; snprintf(b, sizeof(b), "%d", pw->flow_k); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a >= '0' && *a <= '1') { pw->flow_k = *a - '0'; return pw_end(pw, a + 1); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "&D"))) {
        if (*a == '?') { char b[4]; snprintf(b, sizeof(b), "%d", pw->dtr_d); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a >= '0' && *a <= '3') { pw->dtr_d = *a - '0'; return pw_end(pw, a + 1); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "&V"))) {
        if (*a == '0' || *a == '1' || *a == '\0' || *a == ' ') {
            char b[256];
            snprintf(b, sizeof(b), "E%d Q%d V%d X%d &K%d &D%d S0=%d S2=%d NET%d",
                     pw->echo, pw->quiet, pw->verbose, pw->xcode,
                     pw->flow_k, pw->dtr_d, pw->s0_rings, pw->esc_char, pw->telnet);
            pw_qval(pw, b);
            snprintf(b, sizeof(b), "SSID:%s MDNS:%s BAUD:%d FMT:%s TTY:%s %dx%d",
                     pw->ssid, pw->mdns, pw->baud, pw->fmt,
                     pw->tty_type, pw->tty_w, pw->tty_h);
            pw_qval(pw, b);
        }
        if (*a == '0' || *a == '1') a++;
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "&W"))) { return pw_end(pw, a); }   /* save: no-op (sprint 49) */
    if ((a = pw_match(p, "&F"))) { pw_factory(pw, false); return pw_end(pw, a); }

    /* ── Telnet enable ── */
    if ((a = pw_match(p, "NET"))) {
        if (*a == '?') { char b[4]; snprintf(b, sizeof(b), "%d", pw->telnet); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a >= '0' && *a <= '2') { pw->telnet = *a - '0'; return pw_end(pw, a + 1); }
        return pw_end(pw, a);
    }

    /* ── Dialing (consumes rest of line, emits own result) ── */
    if ((a = pw_match(p, "DT")) || (a = pw_match(p, "DP"))) {
        const char* end = a + strlen(a);
        pw_do_dial(pw, a);
        return end;
    }
    if ((a = pw_match(p, "DS"))) {
        int n = (*a >= '0' && *a <= '9') ? (*a - '0') : -1;
        const char* after = pw_skip_digits(a);
        if (n >= 0 && pw->dial[n].used) pw_dial(pw, pw->dial[n].host, pw->dial[n].port);
        else pw_result(pw, PW_ERROR);
        return after;
    }
    if ((a = pw_match(p, "D"))) {
        const char* end = a + strlen(a);
        pw_do_dial(pw, a);
        return end;
    }

    /* ── Network utilities (stubbed — sprint 48) ── */
    if ((a = pw_match(p, "GET"))) {
        pw_qval(pw, "(ATGET not supported in emulation)");
        return pw_end(pw, a + strlen(a));
    }
    if ((a = pw_match(p, "RD")) || (a = pw_match(p, "RT"))) {
        pw_qval(pw, "1970-01-01 00:00:00 UTC (no clock)");
        return pw_end(pw, a);
    }

    /* ── WiFi connection control ── */
    if ((a = pw_match(p, "C"))) {
        if (*a == '?') {
            pw_rx_str(pw, "\r\n");
            pw_rx_push(pw, pw->wifi_up ? '1' : '0');
            pw_rx_str(pw, "\r\n");
            return pw_end(pw, a + 1);
        }
        if (*a == '1') {
            if (pw_wifi_associate(pw)) return pw_end(pw, a + 1);
            pw_result(pw, PW_ERROR);
            return a + 1;
        }
        if (*a == '0') { pw->wifi_up = false; return pw_end(pw, a + 1); }
        pw->wifi_up = false;           /* ATC alone → disconnect */
        return pw_end(pw, a);
    }

    /* ── S-registers ── */
    if ((a = pw_match(p, "S0"))) {
        if (*a == '?') { char b[8]; snprintf(b, sizeof(b), "%d", pw->s0_rings); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a == '=') { pw->s0_rings = atoi(a + 1); return pw_end(pw, pw_skip_digits(a + 1)); }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "S2"))) {
        if (*a == '?') { char b[8]; snprintf(b, sizeof(b), "%d", pw->esc_char); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a == '=') { pw->esc_char = atoi(a + 1); return pw_end(pw, pw_skip_digits(a + 1)); }
        return pw_end(pw, a);
    }

    /* ── Result-code formatting ── */
    if ((a = pw_match(p, "E"))) { pw->echo = (*a != '0'); return pw_end(pw, (*a == '0' || *a == '1') ? a + 1 : a); }
    if ((a = pw_match(p, "Q"))) { pw->quiet = (*a == '1'); return pw_end(pw, (*a == '0' || *a == '1') ? a + 1 : a); }
    if ((a = pw_match(p, "V"))) { pw->verbose = (*a != '0'); return pw_end(pw, (*a == '0' || *a == '1') ? a + 1 : a); }
    if ((a = pw_match(p, "X"))) { pw->xcode = (*a == '0') ? 0 : 1; return pw_end(pw, (*a == '0' || *a == '1') ? a + 1 : a); }

    /* ── Hangup / online / answer / info / reset ── */
    if ((a = pw_match(p, "H"))) {
        if (pw->sockfd >= 0) { close(pw->sockfd); pw->sockfd = -1; log_info("PicoWiFi: hangup"); }
        pw->mode = 0;
        return pw_end(pw, (*a == '0') ? a + 1 : a);
    }
    if ((a = pw_match(p, "O"))) {
        if (pw->sockfd >= 0) { pw->mode = 1; pw_result(pw, PW_CONNECT); }
        else pw_result(pw, PW_NO_CARRIER);
        return a;
    }
    if ((a = pw_match(p, "A"))) {
        pw_result(pw, PW_NO_CARRIER);   /* no incoming call in this emulation */
        return a;
    }
    if ((a = pw_match(p, "I"))) {
        char b[160];
        pw_qval(pw, "PicoWiFiModemUSB (emulated)");
        snprintf(b, sizeof(b), "Firmware v%s", PW_FW_VERSION);
        pw_qval(pw, b);
        snprintf(b, sizeof(b), "WiFi: %s %s",
                 pw->ssid[0] ? pw->ssid : "(not configured)",
                 pw->wifi_up ? "UP" : "DOWN");
        pw_qval(pw, b);
        pw_qval(pw, "IP: 192.168.0.42 (simulated)");
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "Z"))) {
        /* Firmware ATZ is a no-op (a hard reset would drop USB). */
        return pw_end(pw, a);
    }

    return NULL;   /* unknown token */
}

static void pw_process_at(picowifi_t* pw, const char* raw)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", raw);

    /* Trim trailing whitespace, then leading. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';
    char* line = buf;
    while (*line == ' ' || *line == '\t') line++;

    /* A/ repeats the last command (no AT prefix, no CR on real hw). */
    if ((line[0] == 'A' || line[0] == 'a') && line[1] == '/') {
        snprintf(buf, sizeof(buf), "%s", pw->last_cmd);
        line = buf;
    } else {
        snprintf(pw->last_cmd, sizeof(pw->last_cmd), "%s", line);
    }

    /* Mandatory AT prefix. */
    if ((line[0] == 'A' || line[0] == 'a') && (line[1] == 'T' || line[1] == 't')) {
        line += 2;
    } else if (line[0] != '\0') {
        pw_result(pw, PW_ERROR);
        return;
    }

    /* Bare AT → OK */
    if (line[0] == '\0') { pw_result(pw, PW_OK); return; }

    /* Compound command loop: each token consumes its args; only the last
     * emits a result; an unknown token aborts the whole line with ERROR. */
    const char* p = line;
    while (*p) {
        const char* next = pw_dispatch_one(pw, p);
        if (!next || next == p) { pw_result(pw, PW_ERROR); return; }
        p = next;
        while (*p == ' ') p++;
        if (pw->mode == 1) break;   /* a dial switched us to data mode */
    }
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
    pw->mode = 0;
    pw->cmd_len = 0;
    pw->plus_count = 0;
    pw->silence = 0;
    pw->connect_time = 0;
    /* Apply factory defaults, preserving any CLI-supplied credentials
     * (firmware: boot loads NVRAM; we have none yet → factory defaults). */
    pw_factory(pw, true);
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
        /* +++ escape with guard time (escape char is S2-configurable;
         * values >= 128 disable the escape per firmware). Real 1 s guard
         * timing is sprint 47; this is a poll-count approximation. */
        if (pw->esc_char < 128 && (int)byte == pw->esc_char && pw->silence >= 50) {
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
        if (pw->sockfd >= 0 && pw->plus_count > 0) {
            uint8_t e = (uint8_t)pw->esc_char;
            for (int i = 0; i < pw->plus_count && i < 3; i++)
                (void)pw_write(pw->sockfd, &e, 1);
        }
        pw->plus_count = 0;
        pw->silence = 0;

        if (pw->sockfd >= 0) {
            ssize_t r = pw_write(pw->sockfd, &byte, 1);
            if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
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
        ssize_t r = pw_read(pw->sockfd, tmp, sizeof(tmp));
        if (r > 0) {
            for (ssize_t i = 0; i < r; i++) pw_rx_push(pw, tmp[i]);
        } else if (r == 0) {
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

    if (ssid) snprintf(pw->ssid, sizeof(pw->ssid), "%s", ssid);
    if (pass) snprintf(pw->pass, sizeof(pw->pass), "%s", pass);

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
