/**
 * @file gdbstub.c
 * @brief GDB Remote Serial Protocol stub (see gdbstub.h)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * Implements enough of the GDB RSP to debug the 6502 from `gdb`/IDEs:
 *   ?  g G p P  m M  c s  Z0-Z4/z0-z4 (bp + write/read/access watch)  H D k
 *   qSupported qAttached qC qfThreadInfo qsThreadInfo qOffsets qSymbol
 *   qXfer:features:read:target.xml  QStartNoAckMode  vCont
 *
 * Register block order (g/G): A X Y SP PClo PChi P  (7 bytes / 14 hex chars).
 * GDB breakpoints/watchpoints are stored in the shared debugger_t arrays.
 */

#define _POSIX_C_SOURCE 200809L
#include "network/gdbstub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "debugger.h"
#include "utils/logging.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ─── hex helpers ────────────────────────────────────────────────────── */

static char hexdig(int v) { return "0123456789abcdef"[v & 0xF]; }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void emit_byte(char** p, uint8_t b) {
    *(*p)++ = hexdig(b >> 4);
    *(*p)++ = hexdig(b & 0xF);
}

/* Parse a hex number, advancing *s past it. Returns the value; *ok set. */
static uint32_t parse_hex(const char** s, bool* ok) {
    uint32_t v = 0;
    int n = 0;
    int d;
    while ((d = hexval(**s)) >= 0) { v = (v << 4) | (uint32_t)d; (*s)++; n++; }
    if (ok) *ok = (n > 0);
    return v;
}

uint8_t gdb_checksum(const char* data, size_t len) {
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) sum += (unsigned char)data[i];
    return (uint8_t)(sum & 0xFF);
}

/* ─── side-effect-free memory peek (mirror of the debugger's) ─────────── */

static uint8_t gdb_peek(emulator_t* emu, uint16_t a) {
    if (a < RAM_SIZE) return emu->memory.ram[a];
    return memory_read(&emu->memory, a);
}

/* ─── target description ─────────────────────────────────────────────── */

static const char TARGET_XML[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "  <architecture>mos6502</architecture>\n"
    "  <feature name=\"org.gnu.gdb.mos6502.cpu\">\n"
    "    <reg name=\"a\"  bitsize=\"8\"  type=\"int\"/>\n"
    "    <reg name=\"x\"  bitsize=\"8\"  type=\"int\"/>\n"
    "    <reg name=\"y\"  bitsize=\"8\"  type=\"int\"/>\n"
    "    <reg name=\"sp\" bitsize=\"8\"  type=\"data_ptr\"/>\n"
    "    <reg name=\"pc\" bitsize=\"16\" type=\"code_ptr\"/>\n"
    "    <reg name=\"p\"  bitsize=\"8\"  type=\"int\"/>\n"
    "  </feature>\n"
    "</target>\n";

/* ─── register block ─────────────────────────────────────────────────── */

static void regs_to_hex(emulator_t* emu, char* out) {
    char* p = out;
    emit_byte(&p, emu->cpu.A);
    emit_byte(&p, emu->cpu.X);
    emit_byte(&p, emu->cpu.Y);
    emit_byte(&p, emu->cpu.SP);
    emit_byte(&p, (uint8_t)(emu->cpu.PC & 0xFF));   /* PC little-endian */
    emit_byte(&p, (uint8_t)(emu->cpu.PC >> 8));
    emit_byte(&p, emu->cpu.P);
    *p = '\0';
}

static void hex_to_regs(emulator_t* emu, const char* h) {
    uint8_t b[7];
    for (int i = 0; i < 7; i++) {
        int hi = hexval(h[i * 2]), lo = hexval(h[i * 2 + 1]);
        if (hi < 0 || lo < 0) return;
        b[i] = (uint8_t)((hi << 4) | lo);
    }
    emu->cpu.A = b[0]; emu->cpu.X = b[1]; emu->cpu.Y = b[2];
    emu->cpu.SP = b[3];
    emu->cpu.PC = (uint16_t)(b[4] | (b[5] << 8));
    emu->cpu.P = b[6];
}

/* Remove the first breakpoint matching addr. Returns true if removed. */
static bool remove_bp_at(debugger_t* dbg, uint16_t addr) {
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        if (dbg->breakpoints[i].addr == addr)
            return debugger_remove_breakpoint(dbg, i);
    }
    return false;
}

static bool remove_wp_at(debugger_t* dbg, uint16_t addr) {
    for (int i = 0; i < dbg->num_watchpoints; i++) {
        if (dbg->watchpoints[i].addr == addr)
            return debugger_remove_watchpoint(dbg, i);
    }
    return false;
}

/* ─── command dispatch (pure: no socket) ─────────────────────────────── */

gdb_action_t gdb_dispatch(gdb_stub_t* stub, emulator_t* emu,
                          const char* pkt, char* resp, size_t resp_size) {
    resp[0] = '\0';
    debugger_t* dbg = &emu->debugger;

    switch (pkt[0]) {
    case '?':
        snprintf(resp, resp_size, "S%02x", stub->stop_signal);
        return GDB_ACT_NONE;

    case 'g':
        regs_to_hex(emu, resp);
        return GDB_ACT_NONE;

    case 'G':
        hex_to_regs(emu, pkt + 1);
        snprintf(resp, resp_size, "OK");
        return GDB_ACT_NONE;

    case 'p': {                       /* p N — read one register */
        const char* s = pkt + 1;
        bool ok; uint32_t n = parse_hex(&s, &ok);
        char* w = resp;
        if (!ok) { snprintf(resp, resp_size, "E01"); return GDB_ACT_NONE; }
        switch (n) {
        case 0: emit_byte(&w, emu->cpu.A); break;
        case 1: emit_byte(&w, emu->cpu.X); break;
        case 2: emit_byte(&w, emu->cpu.Y); break;
        case 3: emit_byte(&w, emu->cpu.SP); break;
        case 4: emit_byte(&w, (uint8_t)(emu->cpu.PC & 0xFF));
                emit_byte(&w, (uint8_t)(emu->cpu.PC >> 8)); break;
        case 5: emit_byte(&w, emu->cpu.P); break;
        default: snprintf(resp, resp_size, "E02"); return GDB_ACT_NONE;
        }
        *w = '\0';
        return GDB_ACT_NONE;
    }

    case 'P': {                       /* P N=VV — write one register */
        const char* s = pkt + 1;
        bool ok; uint32_t n = parse_hex(&s, &ok);
        if (!ok || *s != '=') { snprintf(resp, resp_size, "E01"); return GDB_ACT_NONE; }
        s++;
        bool vok; uint32_t v = parse_hex(&s, &vok);
        if (!vok) { snprintf(resp, resp_size, "E01"); return GDB_ACT_NONE; }
        /* RSP sends little-endian hex; PC is 16-bit. */
        switch (n) {
        case 0: emu->cpu.A = (uint8_t)v; break;
        case 1: emu->cpu.X = (uint8_t)v; break;
        case 2: emu->cpu.Y = (uint8_t)v; break;
        case 3: emu->cpu.SP = (uint8_t)v; break;
        case 4: emu->cpu.PC = (uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); break;
        case 5: emu->cpu.P = (uint8_t)v; break;
        default: snprintf(resp, resp_size, "E02"); return GDB_ACT_NONE;
        }
        snprintf(resp, resp_size, "OK");
        return GDB_ACT_NONE;
    }

    case 'm': {                       /* m addr,len — read memory */
        const char* s = pkt + 1;
        bool ok1, ok2;
        uint32_t addr = parse_hex(&s, &ok1);
        if (*s == ',') s++;
        uint32_t len = parse_hex(&s, &ok2);
        if (!ok1 || !ok2) { snprintf(resp, resp_size, "E01"); return GDB_ACT_NONE; }
        size_t maxbytes = (resp_size - 1) / 2;
        if (len > maxbytes) len = (uint32_t)maxbytes;
        char* w = resp;
        for (uint32_t i = 0; i < len; i++)
            emit_byte(&w, gdb_peek(emu, (uint16_t)(addr + i)));
        *w = '\0';
        return GDB_ACT_NONE;
    }

    case 'M': {                       /* M addr,len:data — write memory */
        const char* s = pkt + 1;
        bool ok1, ok2;
        uint32_t addr = parse_hex(&s, &ok1);
        if (*s == ',') s++;
        uint32_t len = parse_hex(&s, &ok2);
        if (!ok1 || !ok2 || *s != ':') { snprintf(resp, resp_size, "E01"); return GDB_ACT_NONE; }
        s++;
        for (uint32_t i = 0; i < len; i++) {
            int hi = hexval(s[i * 2]), lo = hexval(s[i * 2 + 1]);
            if (hi < 0 || lo < 0) { snprintf(resp, resp_size, "E01"); return GDB_ACT_NONE; }
            memory_write(&emu->memory, (uint16_t)(addr + i), (uint8_t)((hi << 4) | lo));
        }
        snprintf(resp, resp_size, "OK");
        return GDB_ACT_NONE;
    }

    case 'c': {                       /* c [addr] — continue */
        const char* s = pkt + 1;
        bool ok; uint32_t addr = parse_hex(&s, &ok);
        if (ok) emu->cpu.PC = (uint16_t)addr;
        return GDB_ACT_CONTINUE;
    }

    case 's': {                       /* s [addr] — step */
        const char* s = pkt + 1;
        bool ok; uint32_t addr = parse_hex(&s, &ok);
        if (ok) emu->cpu.PC = (uint16_t)addr;
        return GDB_ACT_STEP;
    }

    case 'Z':                         /* insert breakpoint/watchpoint */
    case 'z': {                       /* remove breakpoint/watchpoint */
        bool insert = (pkt[0] == 'Z');
        char type = pkt[1];
        const char* s = pkt + 2;
        if (*s == ',') s++;
        bool ok; uint32_t addr = parse_hex(&s, &ok);
        if (!ok) { snprintf(resp, resp_size, "E01"); return GDB_ACT_NONE; }
        bool done = false;
        if (type == '0' || type == '1') {           /* sw/hw breakpoint */
            done = insert ? (debugger_add_breakpoint(dbg, (uint16_t)addr) >= 0)
                          : remove_bp_at(dbg, (uint16_t)addr);
        } else if (type == '2' || type == '3' || type == '4') {
            /* Z2 write, Z3 read, Z4 access watchpoint */
            watch_mode_t m = (type == '2') ? WATCH_WRITE
                           : (type == '3') ? WATCH_READ
                                           : WATCH_ACCESS;
            done = insert ? (debugger_add_watchpoint_mode(dbg, (uint16_t)addr, m) >= 0)
                          : remove_wp_at(dbg, (uint16_t)addr);
            /* (Re)install the memory-trace callback so watchpoints actually fire. */
            debugger_install_watchpoint_trace(dbg, emu);
        } else {
            return GDB_ACT_NONE;                      /* unsupported → empty */
        }
        snprintf(resp, resp_size, done ? "OK" : "E01");
        return GDB_ACT_NONE;
    }

    case 'H':                         /* set thread — single-threaded */
        snprintf(resp, resp_size, "OK");
        return GDB_ACT_NONE;

    case 'D':                         /* detach */
        snprintf(resp, resp_size, "OK");
        return GDB_ACT_DETACH;

    case 'k':                         /* kill */
        emu->running = false;
        return GDB_ACT_DETACH;

    case 'q':
        if (strncmp(pkt, "qSupported", 10) == 0) {
            snprintf(resp, resp_size,
                     "PacketSize=1024;qXfer:features:read+;QStartNoAckMode+");
        } else if (strcmp(pkt, "qAttached") == 0) {
            snprintf(resp, resp_size, "1");
        } else if (strcmp(pkt, "qC") == 0) {
            snprintf(resp, resp_size, "QC1");
        } else if (strcmp(pkt, "qfThreadInfo") == 0) {
            snprintf(resp, resp_size, "m01");
        } else if (strcmp(pkt, "qsThreadInfo") == 0) {
            snprintf(resp, resp_size, "l");
        } else if (strcmp(pkt, "qOffsets") == 0) {
            snprintf(resp, resp_size, "Text=0;Data=0;Bss=0");
        } else if (strncmp(pkt, "qSymbol", 7) == 0) {
            snprintf(resp, resp_size, "OK");
        } else if (strncmp(pkt, "qXfer:features:read:target.xml:", 31) == 0) {
            const char* s = pkt + 31;
            bool ok1, ok2;
            uint32_t off = parse_hex(&s, &ok1);
            if (*s == ',') s++;
            uint32_t len = parse_hex(&s, &ok2);
            size_t xml_len = sizeof(TARGET_XML) - 1;
            (void)ok1; (void)ok2;
            if (off >= xml_len) {
                snprintf(resp, resp_size, "l");
            } else {
                size_t avail = xml_len - off;
                size_t chunk = (len < avail) ? len : avail;
                if (chunk > resp_size - 2) chunk = resp_size - 2;
                resp[0] = (off + chunk >= xml_len) ? 'l' : 'm';
                memcpy(resp + 1, TARGET_XML + off, chunk);
                resp[1 + chunk] = '\0';
            }
        }
        /* else: empty (unsupported) */
        return GDB_ACT_NONE;

    case 'Q':
        if (strcmp(pkt, "QStartNoAckMode") == 0) {
            stub->no_ack_mode = true;
            snprintf(resp, resp_size, "OK");
        }
        return GDB_ACT_NONE;

    case 'v':
        if (strcmp(pkt, "vCont?") == 0) {
            snprintf(resp, resp_size, "vCont;c;C;s;S");
            return GDB_ACT_NONE;
        }
        if (strncmp(pkt, "vCont;c", 7) == 0 || strncmp(pkt, "vCont;C", 7) == 0)
            return GDB_ACT_CONTINUE;
        if (strncmp(pkt, "vCont;s", 7) == 0 || strncmp(pkt, "vCont;S", 7) == 0)
            return GDB_ACT_STEP;
        /* vMustReplyEmpty and unknown v packets → empty */
        return GDB_ACT_NONE;

    default:
        return GDB_ACT_NONE;          /* empty reply = unsupported */
    }
}

/* ─── transport ──────────────────────────────────────────────────────── */

static bool send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/* Send "$payload#cs" and (unless no-ack) wait for the '+' acknowledgement. */
static void send_packet(gdb_stub_t* stub, const char* payload) {
    char frame[1100];
    size_t plen = strlen(payload);
    if (plen > sizeof(frame) - 4) plen = sizeof(frame) - 4;
    uint8_t cs = gdb_checksum(payload, plen);
    int n = snprintf(frame, sizeof(frame), "$%.*s#%02x", (int)plen, payload, cs);
    for (int tries = 0; tries < 4; tries++) {
        if (!send_all(stub->conn_fd, frame, (size_t)n)) return;
        if (stub->no_ack_mode) return;
        char ack;
        ssize_t r = recv(stub->conn_fd, &ack, 1, 0);
        if (r <= 0) return;            /* disconnected */
        if (ack == '+') return;
        /* '-' → resend */
    }
}

/* Read one packet payload into buf (NUL-terminated). Returns payload length,
 * 0 if a lone Ctrl-C interrupt was seen, or -1 on disconnect. */
static int read_packet(gdb_stub_t* stub, char* buf, size_t size) {
    int state = 0;          /* 0=waiting '$', 1=payload, 2=cs hi, 3=cs lo */
    size_t len = 0;
    uint8_t want_cs = 0;
    int cs_hi = 0;
    for (;;) {
        char c;
        ssize_t r = recv(stub->conn_fd, &c, 1, 0);
        if (r <= 0) return -1;
        switch (state) {
        case 0:
            if (c == '$') { len = 0; state = 1; }
            else if (c == 0x03) { buf[0] = '\0'; return 0; }  /* interrupt */
            /* ignore stray '+'/'-' */
            break;
        case 1:
            if (c == '#') state = 2;
            else if (len < size - 1) buf[len++] = c;
            break;
        case 2:
            cs_hi = hexval(c); state = 3;
            break;
        case 3: {
            buf[len] = '\0';
            int lo = hexval(c);
            want_cs = (uint8_t)(((cs_hi < 0 ? 0 : cs_hi) << 4) | (lo < 0 ? 0 : lo));
            uint8_t got = gdb_checksum(buf, len);
            if (!stub->no_ack_mode) {
                char ack = (got == want_cs) ? '+' : '-';
                send_all(stub->conn_fd, &ack, 1);
            }
            if (got == want_cs) return (int)len;
            state = 0;                  /* bad checksum → wait for resend */
            break;
        }
        }
    }
}

/* ─── public API ─────────────────────────────────────────────────────── */

bool gdb_stub_init(gdb_stub_t* stub, uint16_t port) {
    memset(stub, 0, sizeof(*stub));
    stub->listen_fd = -1;
    stub->conn_fd = -1;
    stub->port = port ? port : GDB_DEFAULT_PORT;
    stub->stop_signal = 5;             /* SIGTRAP */

    stub->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (stub->listen_fd < 0) {
        log_error("GDB stub: socket() failed: %s", strerror(errno));
        return false;
    }
    int opt = 1;
    setsockopt(stub->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(stub->port);
    if (bind(stub->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("GDB stub: bind() failed on port %d: %s",
                  stub->port, strerror(errno));
        close(stub->listen_fd); stub->listen_fd = -1;
        return false;
    }
    if (listen(stub->listen_fd, 1) < 0) {
        log_error("GDB stub: listen() failed: %s", strerror(errno));
        close(stub->listen_fd); stub->listen_fd = -1;
        return false;
    }

    log_info("GDB stub: waiting for connection on :%d "
             "(gdb: target remote :%d)", stub->port, stub->port);
    stub->conn_fd = accept(stub->listen_fd, NULL, NULL);
    if (stub->conn_fd < 0) {
        log_error("GDB stub: accept() failed: %s", strerror(errno));
        return false;
    }
    /* Low latency for interactive stepping. */
    int nodelay = 1;
    setsockopt(stub->conn_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    stub->attached = true;
    log_info("GDB stub: client connected");
    return true;
}

void gdb_stub_close(gdb_stub_t* stub) {
    if (stub->conn_fd >= 0) { close(stub->conn_fd); stub->conn_fd = -1; }
    if (stub->listen_fd >= 0) { close(stub->listen_fd); stub->listen_fd = -1; }
    stub->attached = false;
}

void gdb_stub_stopped(gdb_stub_t* stub, emulator_t* emu) {
    if (!stub->attached || stub->conn_fd < 0) { emu->gdb_mode = false; return; }

    /* Report the stop that brought us here (after a continue/step). */
    if (stub->resumed) {
        char r[16];
        snprintf(r, sizeof(r), "S%02x", stub->stop_signal);
        send_packet(stub, r);
        stub->resumed = false;
    }

    char pkt[1100];
    char resp[1100];
    for (;;) {
        int n = read_packet(stub, pkt, sizeof(pkt));
        if (n < 0) {                    /* disconnected */
            gdb_stub_close(stub);
            emu->gdb_mode = false;
            emu->debugger.active = false;
            emu->debugger.step_mode = false;
            return;
        }
        if (n == 0) {                   /* lone Ctrl-C while stopped: ignore */
            continue;
        }
        gdb_action_t act = gdb_dispatch(stub, emu, pkt, resp, sizeof(resp));
        if (act == GDB_ACT_CONTINUE || act == GDB_ACT_STEP) {
            stub->resumed = true;
            stub->stop_signal = 5;
            emu->debugger.active = false;
            emu->debugger.step_mode = (act == GDB_ACT_STEP);
            return;
        }
        send_packet(stub, resp);
        if (act == GDB_ACT_DETACH) {
            gdb_stub_close(stub);
            emu->gdb_mode = false;
            emu->debugger.active = false;
            emu->debugger.step_mode = false;
            return;
        }
        /* GDB_ACT_NONE → keep serving */
    }
}

bool gdb_stub_poll_interrupt(gdb_stub_t* stub) {
    if (stub->conn_fd < 0) return false;
    char c;
    ssize_t n = recv(stub->conn_fd, &c, 1, MSG_DONTWAIT);
    if (n == 0) return true;            /* disconnect → stop and clean up */
    if (n > 0 && c == 0x03) {
        stub->stop_signal = 2;          /* SIGINT */
        return true;
    }
    return false;
}
