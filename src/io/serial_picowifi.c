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
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "io/serial_backend.h"
#include "utils/logging.h"

#define PW_FW_VERSION   "0.1.0"
#define PW_RX_BUFSZ     65536      /* 64KB receive ring (line → Oric) */
#define PW_DIAL_SLOTS   10         /* AT&Z0..9 speed-dial entries */
#define PW_DEFAULT_PORT 23         /* telnet */
#define PW_DEFAULT_BAUD 9600       /* firmware DEFAULT_SPEED */

/* Telnet protocol (sprint 47). Modes: 0=none, 1=real (CR+NUL), 2=fake. */
#define TN_NONE   0
#define TN_REAL   1
#define TN_FAKE   2
/* Commands */
#define TN_SE    240
#define TN_NOP   241
#define TN_DM    242
#define TN_BRK   243
#define TN_AYT   246
#define TN_SB    250
#define TN_WILL  251
#define TN_WONT  252
#define TN_DO    253
#define TN_DONT  254
#define TN_IAC   255
/* Options */
#define TNO_BINARY      0
#define TNO_ECHO        1
#define TNO_SUP_GA      3
#define TNO_LOC        23
#define TNO_TTYPE      24
#define TNO_NAWS       31
#define TNO_TSPEED     32
#define TNO_LFLOW      33
#define TNO_LINEMODE   34
#define TNO_XDISPLOC   35
#define TNO_NEW_ENVIRON 39
/* Receive state machine */
#define TN_ST_NORMAL  0
#define TN_ST_IAC     1
#define TN_ST_VERB    2   /* got WILL/WONT/DO/DONT, awaiting option */
#define TN_ST_SB      3   /* in subnegotiation, awaiting IAC SE */
#define TN_ST_SB_IAC  4   /* saw IAC inside subnegotiation */

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
    bool     wifi_up;               /* association state */
    bool     realnet;              /* reflect the host's real network state */

    /* ── Serial parameters (reported, not enforced) ── */
    int      baud;                  /* AT$SB, default 9600 */
    char     fmt[8];                /* AT$SU, default "8N1" */

    /* ── Telnet ── */
    int      telnet;                /* ATNET: configured default (0/1/2) */
    int      session_telnet;        /* active mode for the current call */
    int      tn_state;              /* receive state machine (TN_ST_*) */
    int      tn_verb;               /* pending WILL/WONT/DO/DONT */
    int      tn_sb_opt;             /* subnegotiation option (-1 = none yet) */
    bool     tn_prev_cr;            /* previous delivered byte was CR */
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
    char     busy_msg[128];         /* $BM, sent to rejected callers */

    /* ── Startup ── */
    char     auto_exec[256];        /* $AE */
    int      startup_wait;          /* $W */

    /* ── NVRAM (sprint 49): host file standing in for the Pico flash ── */
    char     nvram_path[512];       /* resolved at open(); "" = no persistence */
    char     cli_ssid[64];          /* credentials from the CLI (override NVRAM) */
    char     cli_pass[64];
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
    /* Firmware: a lost/failed call resets the session telnet mode to the
     * configured default and clears the receive state machine. */
    if (code == PW_NO_CARRIER || code == PW_NO_ANSWER) {
        pw->session_telnet = pw->telnet;
        pw->tn_state = TN_ST_NORMAL;
        pw->tn_prev_cr = false;
    }

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
    pw->telnet = TN_REAL;
    pw->session_telnet = TN_REAL;
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
    snprintf(pw->busy_msg, sizeof(pw->busy_msg),
             "Sorry, the system is currently busy. Please try again later.");
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
 *  NVRAM persistence (sprint 49) — a host file stands in for the Pico
 *  flash. The real firmware writes SETTINGS_T verbatim; we use a robust
 *  key=value text format with a "PWNV1" magic header.
 * ═══════════════════════════════════════════════════════════════════════ */

static void pw_parse_hostport(const char* s, char* host, size_t hsz, uint16_t* port);

/* Resolve the NVRAM file path: PHOSPHORIC_PICOWIFI_NVRAM env var, else
 * $HOME/.phosphoric_picowifi.cfg, else disabled. */
static void pw_nvram_resolve_path(picowifi_t* pw)
{
    const char* env = getenv("PHOSPHORIC_PICOWIFI_NVRAM");
    if (env && env[0]) {
        snprintf(pw->nvram_path, sizeof(pw->nvram_path), "%s", env);
        return;
    }
    const char* home = getenv("HOME");
    if (home && home[0])
        snprintf(pw->nvram_path, sizeof(pw->nvram_path), "%s/.phosphoric_picowifi.cfg", home);
    else
        pw->nvram_path[0] = '\0';
}

static bool pw_nvram_save(picowifi_t* pw)
{
    if (pw->nvram_path[0] == '\0') return false;
    FILE* f = fopen(pw->nvram_path, "w");
    if (!f) return false;
    fprintf(f, "PWNV1\n");
    fprintf(f, "ssid=%s\n", pw->ssid);
    fprintf(f, "pass=%s\n", pw->pass);
    fprintf(f, "mdns=%s\n", pw->mdns);
    fprintf(f, "baud=%d\n", pw->baud);
    fprintf(f, "fmt=%s\n", pw->fmt);
    fprintf(f, "telnet=%d\n", pw->telnet);
    fprintf(f, "tty_type=%s\n", pw->tty_type);
    fprintf(f, "tty_w=%d\n", pw->tty_w);
    fprintf(f, "tty_h=%d\n", pw->tty_h);
    fprintf(f, "tty_loc=%s\n", pw->tty_loc);
    fprintf(f, "s0=%d\n", pw->s0_rings);
    fprintf(f, "s2=%d\n", pw->esc_char);
    fprintf(f, "echo=%d\n", pw->echo);
    fprintf(f, "quiet=%d\n", pw->quiet);
    fprintf(f, "verbose=%d\n", pw->verbose);
    fprintf(f, "xcode=%d\n", pw->xcode);
    fprintf(f, "flow_k=%d\n", pw->flow_k);
    fprintf(f, "dtr_d=%d\n", pw->dtr_d);
    fprintf(f, "server_port=%u\n", pw->server_port);
    fprintf(f, "server_pass=%s\n", pw->server_pass);
    fprintf(f, "busy_msg=%s\n", pw->busy_msg);
    fprintf(f, "auto_exec=%s\n", pw->auto_exec);
    fprintf(f, "startup_wait=%d\n", pw->startup_wait);
    for (int i = 0; i < PW_DIAL_SLOTS; i++) {
        if (pw->dial[i].used)
            fprintf(f, "dial%d=%s:%u,%s\n", i,
                    pw->dial[i].host, pw->dial[i].port, pw->dial[i].alias);
    }
    fclose(f);
    return true;
}

/* Load stored settings into dst's config fields. Returns true if a valid
 * NVRAM file was read. */
static bool pw_nvram_load_into(const char* path, picowifi_t* dst)
{
    if (!path || !path[0]) return false;
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[600];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "PWNV1", 5) != 0) {
        fclose(f);
        return false;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* k = line;
        const char* v = eq + 1;
        if      (!strcmp(k, "ssid"))        snprintf(dst->ssid, sizeof(dst->ssid), "%s", v);
        else if (!strcmp(k, "pass"))        snprintf(dst->pass, sizeof(dst->pass), "%s", v);
        else if (!strcmp(k, "mdns"))        snprintf(dst->mdns, sizeof(dst->mdns), "%s", v);
        else if (!strcmp(k, "baud"))        dst->baud = atoi(v);
        else if (!strcmp(k, "fmt"))         snprintf(dst->fmt, sizeof(dst->fmt), "%s", v);
        else if (!strcmp(k, "telnet"))      dst->telnet = atoi(v);
        else if (!strcmp(k, "tty_type"))    snprintf(dst->tty_type, sizeof(dst->tty_type), "%s", v);
        else if (!strcmp(k, "tty_w"))       dst->tty_w = atoi(v);
        else if (!strcmp(k, "tty_h"))       dst->tty_h = atoi(v);
        else if (!strcmp(k, "tty_loc"))     snprintf(dst->tty_loc, sizeof(dst->tty_loc), "%s", v);
        else if (!strcmp(k, "s0"))          dst->s0_rings = atoi(v);
        else if (!strcmp(k, "s2"))          dst->esc_char = atoi(v);
        else if (!strcmp(k, "echo"))        dst->echo = atoi(v) != 0;
        else if (!strcmp(k, "quiet"))       dst->quiet = atoi(v) != 0;
        else if (!strcmp(k, "verbose"))     dst->verbose = atoi(v) != 0;
        else if (!strcmp(k, "xcode"))       dst->xcode = atoi(v);
        else if (!strcmp(k, "flow_k"))      dst->flow_k = atoi(v);
        else if (!strcmp(k, "dtr_d"))       dst->dtr_d = atoi(v);
        else if (!strcmp(k, "server_port")) dst->server_port = (uint16_t)atoi(v);
        else if (!strcmp(k, "server_pass")) snprintf(dst->server_pass, sizeof(dst->server_pass), "%s", v);
        else if (!strcmp(k, "busy_msg"))    snprintf(dst->busy_msg, sizeof(dst->busy_msg), "%s", v);
        else if (!strcmp(k, "auto_exec"))   snprintf(dst->auto_exec, sizeof(dst->auto_exec), "%s", v);
        else if (!strcmp(k, "startup_wait")) dst->startup_wait = atoi(v);
        else if (!strncmp(k, "dial", 4)) {
            int idx = atoi(k + 4);
            if (idx >= 0 && idx < PW_DIAL_SLOTS) {
                char hp[300] = {0}, alias[32] = {0};
                const char* comma = strrchr(v, ',');
                if (comma) {
                    size_t hl = (size_t)(comma - v);
                    if (hl >= sizeof(hp)) hl = sizeof(hp) - 1;
                    memcpy(hp, v, hl); hp[hl] = '\0';
                    snprintf(alias, sizeof(alias), "%s", comma + 1);
                } else {
                    snprintf(hp, sizeof(hp), "%s", v);
                }
                char host[256]; uint16_t port;
                pw_parse_hostport(hp, host, sizeof(host), &port);
                snprintf(dst->dial[idx].host, sizeof(dst->dial[idx].host), "%s", host);
                dst->dial[idx].port = port;
                snprintf(dst->dial[idx].alias, sizeof(dst->dial[idx].alias), "%s", alias);
                dst->dial[idx].used = true;
            }
        }
    }
    fclose(f);
    return true;
}

/* Emit the two-line settings summary (AT&V) from a source config. */
static void pw_display_settings(picowifi_t* pw, const picowifi_t* s)
{
    char b[256];
    snprintf(b, sizeof(b), "E%d Q%d V%d X%d &K%d &D%d S0=%d S2=%d NET%d",
             s->echo, s->quiet, s->verbose, s->xcode,
             s->flow_k, s->dtr_d, s->s0_rings, s->esc_char, s->telnet);
    pw_qval(pw, b);
    snprintf(b, sizeof(b), "SSID:%s MDNS:%s BAUD:%d FMT:%s TTY:%s %dx%d",
             s->ssid, s->mdns, s->baud, s->fmt, s->tty_type, s->tty_w, s->tty_h);
    pw_qval(pw, b);
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

/* Read from fd until a newline or timeout (ms). Returns bytes read (NUL
 * terminated), or -1 on error/timeout with no data. Socket is non-blocking
 * (pw_tcp_connect sets O_NONBLOCK), so we poll between reads. */
static int pw_read_line_timeout(int fd, char* buf, size_t sz, int timeout_ms)
{
    size_t n = 0;
    int waited = 0;
    while (n < sz - 1) {
        char c;
        ssize_t r = pw_read(fd, &c, 1);
        if (r == 1) {
            if (c == '\n' && n > 0) break;   /* end of line */
            if (c != '\r') buf[n++] = c;     /* keep, drop CR */
            waited = 0;
            continue;
        }
        if (r == 0) break;                   /* peer closed */
        if (waited >= timeout_ms) break;     /* timeout */
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        poll(&pfd, 1, 50);
        waited += 50;
    }
    buf[n] = '\0';
    return (n > 0) ? (int)n : -1;
}

/* Parse a NIST DAYTIME line (RFC 867) into "YY-MM-DD HH:MM:SS".
 * Format: "MJD YY-MM-DD HH:MM:SS TT L H msADV UTC(NIST) OTM".
 * Non-static so the unit tests can exercise it directly. */
bool pw_parse_daytime(const char* raw, char* out, size_t osz)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", raw);
    char* save = NULL;
    char* mjd  = strtok_r(tmp, " \t\r\n", &save);
    char* date = strtok_r(NULL, " \t\r\n", &save);
    char* tm   = strtok_r(NULL, " \t\r\n", &save);
    if (!mjd || !date || !tm) return false;
    snprintf(out, osz, "%s %s", date, tm);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Telnet protocol (sprint 47) — mirrors firmware support.h
 * ═══════════════════════════════════════════════════════════════════════ */

/* Raw best-effort write to the data socket (no transform). Returns false
 * on a fatal (non-EAGAIN) error so the caller can drop carrier. */
static bool pw_sock_write(picowifi_t* pw, const uint8_t* buf, size_t len)
{
    if (pw->sockfd < 0) return true;
    ssize_t r = pw_write(pw->sockfd, buf, len);
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    return true;
}

/* Send one data byte from the Oric to the line, telnet-encoded:
 * IAC doubled (real+fake), CR→CR+NUL (real only). */
static bool pw_tn_out_byte(picowifi_t* pw, uint8_t b)
{
    if (pw->session_telnet != TN_NONE && b == TN_IAC) {
        uint8_t d[2] = { TN_IAC, TN_IAC };
        return pw_sock_write(pw, d, 2);
    }
    if (pw->session_telnet == TN_REAL && b == 0x0D) {
        uint8_t d[2] = { 0x0D, 0x00 };
        return pw_sock_write(pw, d, 2);
    }
    return pw_sock_write(pw, &b, 1);
}

/* Respond to an inbound DO/WILL negotiation. */
static void pw_tn_negotiate(picowifi_t* pw, int verb, int opt)
{
    uint8_t r[3] = { TN_IAC, 0, (uint8_t)opt };
    if (verb == TN_DO) {
        if (opt == TNO_BINARY || opt == TNO_ECHO || opt == TNO_SUP_GA ||
            opt == TNO_TTYPE  || opt == TNO_TSPEED) {
            r[1] = TN_WILL; pw_sock_write(pw, r, 3);
        } else if (opt == TNO_LOC) {
            r[1] = TN_WILL; pw_sock_write(pw, r, 3);
            uint8_t h[3] = { TN_IAC, TN_SB, TNO_LOC };
            uint8_t t[2] = { TN_IAC, TN_SE };
            pw_sock_write(pw, h, 3);
            pw_sock_write(pw, (const uint8_t*)pw->tty_loc, strlen(pw->tty_loc));
            pw_sock_write(pw, t, 2);
        } else if (opt == TNO_NAWS) {
            r[1] = TN_WILL; pw_sock_write(pw, r, 3);
            uint8_t s[9] = { TN_IAC, TN_SB, TNO_NAWS,
                             0, (uint8_t)pw->tty_w, 0, (uint8_t)pw->tty_h,
                             TN_IAC, TN_SE };
            pw_sock_write(pw, s, 9);
        } else {
            r[1] = TN_WONT; pw_sock_write(pw, r, 3);
        }
    } else if (verb == TN_WILL) {
        if (opt == TNO_LINEMODE || opt == TNO_NAWS || opt == TNO_LFLOW ||
            opt == TNO_NEW_ENVIRON || opt == TNO_XDISPLOC) {
            r[1] = TN_DONT; pw_sock_write(pw, r, 3);
        } else {
            r[1] = TN_DO; pw_sock_write(pw, r, 3);
        }
    }
    /* DONT / WONT inbound → ignored */
}

/* Respond to a completed inbound subnegotiation (IAC SB opt ... IAC SE). */
static void pw_tn_subneg(picowifi_t* pw, int opt)
{
    if (opt == TNO_TTYPE) {
        uint8_t h[4] = { TN_IAC, TN_SB, TNO_TTYPE, 0 /* IS */ };
        uint8_t t[2] = { TN_IAC, TN_SE };
        pw_sock_write(pw, h, 4);
        pw_sock_write(pw, (const uint8_t*)pw->tty_type, strlen(pw->tty_type));
        pw_sock_write(pw, t, 2);
    } else if (opt == TNO_TSPEED) {
        char sp[24];
        int n = snprintf(sp, sizeof(sp), "%d,%d", pw->baud, pw->baud);
        uint8_t h[4] = { TN_IAC, TN_SB, TNO_TSPEED, 0 /* IS */ };
        uint8_t t[2] = { TN_IAC, TN_SE };
        pw_sock_write(pw, h, 4);
        pw_sock_write(pw, (const uint8_t*)sp, (size_t)n);
        pw_sock_write(pw, t, 2);
    }
    /* other options → ignored */
}

/* Feed one byte received from the line through the telnet state machine.
 * Data bytes are pushed to the RX ring; telnet control is consumed (and may
 * write responses back to the line). */
static void pw_tn_rx_byte(picowifi_t* pw, uint8_t b)
{
    switch (pw->tn_state) {
    case TN_ST_NORMAL:
        if (b == TN_IAC) { pw->tn_state = TN_ST_IAC; return; }
        if (pw->session_telnet == TN_REAL && pw->tn_prev_cr && b == 0x00) {
            pw->tn_prev_cr = false;   /* filter CR+NUL → CR */
            return;
        }
        pw_rx_push(pw, b);
        pw->tn_prev_cr = (b == 0x0D);
        return;
    case TN_ST_IAC:
        if (b == TN_IAC) { pw_rx_push(pw, 0xFF); pw->tn_prev_cr = false; pw->tn_state = TN_ST_NORMAL; return; }
        if (b == TN_WILL || b == TN_WONT || b == TN_DO || b == TN_DONT) { pw->tn_verb = b; pw->tn_state = TN_ST_VERB; return; }
        if (b == TN_SB) { pw->tn_sb_opt = -1; pw->tn_state = TN_ST_SB; return; }
        if (b == TN_AYT) { pw_sock_write(pw, (const uint8_t*)"\r\n[Yes]\r\n", 9); pw->tn_state = TN_ST_NORMAL; return; }
        pw->tn_state = TN_ST_NORMAL;   /* DM/NOP/BRK/GA/… ignored */
        return;
    case TN_ST_VERB:
        pw_tn_negotiate(pw, pw->tn_verb, b);
        pw->tn_state = TN_ST_NORMAL;
        return;
    case TN_ST_SB:
        if (b == TN_IAC) { pw->tn_state = TN_ST_SB_IAC; return; }
        if (pw->tn_sb_opt < 0) pw->tn_sb_opt = b;   /* first byte = option */
        return;
    case TN_ST_SB_IAC:
        if (b == TN_SE) { pw_tn_subneg(pw, pw->tn_sb_opt); pw->tn_state = TN_ST_NORMAL; return; }
        pw->tn_state = TN_ST_SB;   /* IAC IAC inside SB = data, ignore */
        return;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Host network reflection (real bridge, read-only) — opt-in via
 *  PHOSPHORIC_PICOWIFI_REALNET=1. We never control the host WiFi card; we
 *  only report its real state (local IP, internet reachability, SSID).
 * ═══════════════════════════════════════════════════════════════════════ */

/* First non-loopback IPv4 address of the host, else loopback/0.0.0.0. */
static void pw_host_local_ip(char* out, size_t osz)
{
    snprintf(out, osz, "0.0.0.0");
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) != 0) return;
    char loop[INET_ADDRSTRLEN] = {0};
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in* sin = (struct sockaddr_in*)p->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        if (strcmp(ip, "127.0.0.1") == 0) { snprintf(loop, sizeof(loop), "%s", ip); continue; }
        snprintf(out, osz, "%s", ip);   /* first real address wins */
        freeifaddrs(ifa);
        return;
    }
    if (loop[0]) snprintf(out, osz, "%s", loop);
    freeifaddrs(ifa);
}

/* True if the host has a route to the internet. Uses the classic "UDP
 * connect to 8.8.8.8" trick — no packet is sent, the kernel just resolves
 * the outbound route (fails with no route when offline). */
static bool pw_host_online(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &sa.sin_addr);
    bool ok = (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0);
    if (ok) {
        struct sockaddr_in local;
        socklen_t l = sizeof(local);
        if (getsockname(fd, (struct sockaddr*)&local, &l) == 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
            ok = (strcmp(ip, "0.0.0.0") != 0 && strcmp(ip, "127.0.0.1") != 0);
        }
    }
    close(fd);
    return ok;
}

/* Best-effort host WiFi SSID (Linux wireless-tools). Empty if unavailable. */
static bool pw_host_ssid(char* out, size_t osz)
{
    out[0] = '\0';
    FILE* p = popen("iwgetid -r 2>/dev/null", "r");
    if (!p) return false;
    bool ok = false;
    if (fgets(out, (int)osz, p)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
        ok = (n > 0);
    }
    pclose(p);
    return ok;
}

static bool pw_wifi_associate(picowifi_t* pw)
{
    if (pw->realnet) {
        /* Real bridge: "association" succeeds iff the host is online. */
        pw->wifi_up = pw_host_online();
        return pw->wifi_up;
    }
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
    pw->tn_state = TN_ST_NORMAL;
    pw->tn_prev_cr = false;
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

/* Dial from a raw argument (consumes the rest of the line). Leading telnet
 * prefixes set the session telnet mode: '-'=none, '='=real, '+'=fake. */
static void pw_do_dial(picowifi_t* pw, const char* arg)
{
    int sess = pw->telnet;
    for (;;) {
        if (*arg == '-')      { sess = TN_NONE; arg++; }
        else if (*arg == '=') { sess = TN_REAL; arg++; }
        else if (*arg == '+') { sess = TN_FAKE; arg++; }
        else if (*arg == ' ') { arg++; }
        else break;
    }
    bool had_colon = (strchr(arg, ':') != NULL);
    char host[256]; uint16_t port;
    pw_parse_hostport(arg, host, sizeof(host), &port);

    /* No explicit port → try speed-dial resolution (firmware dialNumber):
     * a 7-digit run of identical digits selects slot N, otherwise the
     * host is matched against the stored aliases. */
    if (!had_colon && host[0] != '\0') {
        int slot = -1;
        size_t hl = strlen(host);
        if (hl == 7) {
            bool same = true;
            for (size_t i = 0; i < 7; i++)
                if (!isdigit((unsigned char)host[i]) || host[i] != host[0]) { same = false; break; }
            if (same) slot = host[0] - '0';
        }
        if (slot < 0) {
            for (int i = 0; i < PW_DIAL_SLOTS; i++)
                if (pw->dial[i].used && strcasecmp(host, pw->dial[i].alias) == 0) { slot = i; break; }
        }
        if (slot >= 0 && pw->dial[slot].used) {
            snprintf(host, sizeof(host), "%s", pw->dial[slot].host);
            port = pw->dial[slot].port;
        }
    }

    if (host[0] == '\0' && pw->host[0] != '\0') {  /* redial preset */
        snprintf(host, sizeof(host), "%s", pw->host);
        port = pw->port ? pw->port : PW_DEFAULT_PORT;
    }
    pw->session_telnet = sess;
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

/* Case-SENSITIVE variant, used for the ATDT / ATDP dial modifiers.
 *
 * The dial string of a WiFi modem is a hostname, not a phone number, and a
 * host may legitimately start with 'p'/'t' (e.g. "pavi.3617.fr"). Matching
 * the T/P modifier case-insensitively would swallow that leading letter:
 * "ATDpavi…" would parse as DP (pulse) + "avi…". By requiring an UPPERCASE
 * T/P for the modifier we keep the conventional "ATDThost" / "ATDPhost"
 * working while letting a lowercase-led host pass through to the bare-D path
 * untouched. */
static const char* pw_match_cs(const char* s, const char* prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) == 0) return s + n;
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

/* AT$SCAN — list nearby WiFi networks, mirroring the firmware doScan():
 * one access point per line as "<index> <ssid>" (1-based), de-duplicated by
 * SSID, hidden (empty) SSIDs skipped, then OK. The emulator runs on a desktop,
 * so we scan the host's real networks via nmcli, with a simulated fallback when
 * nmcli is absent (CI/headless) so the command always returns something usable. */
static void pw_scan(picowifi_t* pw)
{
    char seen[24][33];
    int  n = 0;
    FILE* f = popen("nmcli -t -f SSID dev wifi 2>/dev/null", "r");
    if (f) {
        char line[256];
        while (n < 24 && fgets(line, sizeof line, f)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
            if (!line[0]) continue;                 /* hidden SSID */
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (!strcmp(seen[i], line)) { dup = 1; break; }
            if (dup) continue;
            snprintf(seen[n], sizeof seen[n], "%s", line);
            n++;
        }
        pclose(f);
    }
    if (n == 0) {                                   /* fallback simulated list */
        static const char* sim[] = { "AlterOP New", "Livebox-E380", "Freebox-39030C" };
        for (int i = 0; i < 3; i++) snprintf(seen[i], sizeof seen[i], "%s", sim[i]);
        n = 3;
    }
    for (int i = 0; i < n; i++) {
        char b[64];
        snprintf(b, sizeof b, "%d %.32s\r\n", i + 1, seen[i]);
        pw_rx_str(pw, b);
    }
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
    if ((a = pw_match(p, "$AYT"))) {
        /* Send Telnet "Are You There" if in a telnet call. */
        if (pw->sockfd >= 0 && pw->session_telnet != TN_NONE) {
            uint8_t s[2] = { TN_IAC, TN_AYT };
            pw_sock_write(pw, s, 2);
            pw->mode = 1;          /* back online */
            return a;              /* no OK: we are online again */
        }
        pw_result(pw, PW_ERROR);
        return a;
    }
    if ((a = pw_match(p, "$SCAN"))) {
        /* No collision with $SSID/$SB/$SU/$SP: pw_match is a prefix test and
         * "$SCAN" diverges at the 3rd char. List APs, then OK. */
        pw_scan(pw);
        return pw_end(pw, a);
    }
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
    if ((a = pw_match(p, "$BM"))) {
        if (*a == '?') { pw_qval(pw, pw->busy_msg); return pw_end(pw, a + 1); }
        if (*a == '=') { return pw_end(pw, pw_setstr(pw->busy_msg, sizeof(pw->busy_msg), a + 1)); }
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
        if (*a == '1') {
            /* AT&V1: show the settings stored in NVRAM (defaults if none). */
            static picowifi_t tmp;   /* large; avoid a big stack frame */
            memset(&tmp, 0, sizeof(tmp));
            pw_factory(&tmp, false);
            pw_nvram_load_into(pw->nvram_path, &tmp);
            pw_display_settings(pw, &tmp);
            a++;
        } else {
            pw_display_settings(pw, pw);   /* AT&V / AT&V0: current settings */
            if (*a == '0') a++;
        }
        return pw_end(pw, a);
    }
    if ((a = pw_match(p, "&W"))) { pw_nvram_save(pw); return pw_end(pw, a); }
    if ((a = pw_match(p, "&F"))) { pw_factory(pw, false); pw_nvram_save(pw); return pw_end(pw, a); }

    /* ── Telnet enable ── */
    if ((a = pw_match(p, "NET"))) {
        if (*a == '?') { char b[4]; snprintf(b, sizeof(b), "%d", pw->telnet); pw_qval(pw, b); return pw_end(pw, a + 1); }
        if (*a >= '0' && *a <= '2') { pw->telnet = *a - '0'; return pw_end(pw, a + 1); }
        return pw_end(pw, a);
    }

    /* ── Dialing (consumes rest of line, emits own result) ── */
    if ((a = pw_match_cs(p, "DT")) || (a = pw_match_cs(p, "DP"))) {
        const char* end = a + strlen(a);
        pw_do_dial(pw, a);
        return end;
    }
    if ((a = pw_match(p, "DS"))) {
        int n = (*a >= '0' && *a <= '9') ? (*a - '0') : -1;
        const char* after = pw_skip_digits(a);
        if (n >= 0 && pw->dial[n].used) {
            pw->session_telnet = pw->telnet;   /* speed dial uses default mode */
            pw_dial(pw, pw->dial[n].host, pw->dial[n].port);
        } else {
            pw_result(pw, PW_ERROR);
        }
        return after;
    }
    if ((a = pw_match(p, "D"))) {
        const char* end = a + strlen(a);
        pw_do_dial(pw, a);
        return end;
    }

    /* ── Network utilities (sprint 48) ── */
    if ((a = pw_match(p, "GET"))) {
        /* ATGET http://host[:port][/path] — HTTP/1.1 GET, raw stream out. */
        const char* url = a;
        const char* end = a + strlen(a);
        while (*url == ' ') url++;
        const char* h = pw_match(url, "http://");
        if (h) url = h;
        char hostport[300] = {0}, path[256] = {0};
        const char* slash = strchr(url, '/');
        if (slash) {
            size_t hl = (size_t)(slash - url);
            if (hl >= sizeof(hostport)) hl = sizeof(hostport) - 1;
            memcpy(hostport, url, hl); hostport[hl] = '\0';
            snprintf(path, sizeof(path), "%s", slash + 1);
        } else {
            snprintf(hostport, sizeof(hostport), "%s", url);
        }
        char host[256]; uint16_t port;
        pw_parse_hostport(hostport, host, sizeof(host), &port);
        if (!strchr(hostport, ':')) port = 80;        /* HTTP default */
        pw->session_telnet = TN_NONE;                 /* raw HTTP, no telnet */
        pw_dial(pw, host, port);                      /* emits CONNECT, mode=1 */
        if (pw->sockfd >= 0) {
            char req[700];
            int rn = snprintf(req, sizeof(req),
                              "GET /%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                              path, host);
            pw_sock_write(pw, (const uint8_t*)req, (size_t)rn);
        }
        return end;
    }
    if ((a = pw_match(p, "RD")) || (a = pw_match(p, "RT"))) {
        /* NIST DAYTIME (RFC 867) at time.nist.gov:13 → "YY-MM-DD HH:MM:SS". */
        if (pw->sockfd >= 0) { pw_result(pw, PW_ERROR); return a; }  /* in call */
        int fd = pw_tcp_connect("time.nist.gov", 13);
        if (fd < 0) { pw_result(pw, PW_ERROR); return a; }
        char line[256];
        /* DAYTIME starts with a blank line, then the timestamp line. */
        int got = pw_read_line_timeout(fd, line, sizeof(line), 2000);
        if (got <= 0) got = pw_read_line_timeout(fd, line, sizeof(line), 2000);
        close(fd);
        char out[64];
        if (got > 0 && pw_parse_daytime(line, out, sizeof(out))) {
            pw_qval(pw, out);
            return pw_end(pw, a);
        }
        pw_result(pw, PW_ERROR);
        return a;
    }

    /* ── WiFi connection control ── */
    if ((a = pw_match(p, "C"))) {
        if (*a == '?') {
            if (pw->realnet) pw->wifi_up = pw_host_online();   /* live state */
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
        if (pw->realnet) {
            bool up = pw_host_online();
            pw->wifi_up = up;
            snprintf(b, sizeof(b), "WiFi: %s %s",
                     pw->ssid[0] ? pw->ssid : "(host)", up ? "UP" : "DOWN");
            pw_qval(pw, b);
            char ip[INET_ADDRSTRLEN];
            pw_host_local_ip(ip, sizeof(ip));
            snprintf(b, sizeof(b), "IP: %s (host)", ip);
            pw_qval(pw, b);
        } else {
            snprintf(b, sizeof(b), "WiFi: %s %s",
                     pw->ssid[0] ? pw->ssid : "(not configured)",
                     pw->wifi_up ? "UP" : "DOWN");
            pw_qval(pw, b);
            pw_qval(pw, "IP: 192.168.0.42 (simulated)");
        }
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
    pw->tn_state = TN_ST_NORMAL;
    pw->tn_prev_cr = false;

    /* Boot sequence (firmware setup): factory defaults, then overlay NVRAM
     * if present, then CLI credentials take precedence. */
    pw_nvram_resolve_path(pw);
    pw_factory(pw, false);
    pw_nvram_load_into(pw->nvram_path, pw);
    if (pw->cli_ssid[0]) snprintf(pw->ssid, sizeof(pw->ssid), "%s", pw->cli_ssid);
    if (pw->cli_pass[0]) snprintf(pw->pass, sizeof(pw->pass), "%s", pw->cli_pass);

    /* Real bridge (opt-in): reflect the host's network state instead of
     * simulating. We read it (SSID, reachability) but never control it. */
    const char* rn = getenv("PHOSPHORIC_PICOWIFI_REALNET");
    pw->realnet = (rn && rn[0] && rn[0] != '0');

    if (pw->realnet) {
        if (pw->ssid[0] == '\0') {
            char real_ssid[64];
            if (pw_host_ssid(real_ssid, sizeof(real_ssid)))
                snprintf(pw->ssid, sizeof(pw->ssid), "%s", real_ssid);
        }
        pw->wifi_up = pw_host_online();
        log_info("PicoWiFi: real host bridge (SSID:%s online:%d)",
                 pw->ssid[0] ? pw->ssid : "(unknown)", pw->wifi_up);
    } else if (pw->ssid[0]) {
        /* Auto-reconnect WiFi at boot when an SSID is configured (simulated). */
        pw->wifi_up = true;
        log_info("PicoWiFi: boot WiFi associate \"%s\" (simulated)", pw->ssid);
    }

    log_info("PicoWiFi: backend opened (SSID:%s)",
             pw->ssid[0] ? pw->ssid : "(unset)");

    /* Run the boot auto-execute command if configured ($AE). */
    if (pw->auto_exec[0])
        pw_process_at(pw, pw->auto_exec);

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
                (void)pw_tn_out_byte(pw, e);
        }
        pw->plus_count = 0;
        pw->silence = 0;

        if (pw->sockfd >= 0) {
            if (!pw_tn_out_byte(pw, byte)) {
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
            for (ssize_t i = 0; i < r; i++) {
                if (pw->session_telnet != TN_NONE) pw_tn_rx_byte(pw, tmp[i]);
                else pw_rx_push(pw, tmp[i]);
            }
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

    /* Stash CLI credentials; open() applies them after loading NVRAM so
     * they take precedence over stored values. */
    if (ssid) snprintf(pw->cli_ssid, sizeof(pw->cli_ssid), "%s", ssid);
    if (pass) snprintf(pw->cli_pass, sizeof(pw->cli_pass), "%s", pass);

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
