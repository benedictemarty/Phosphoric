/**
 * @file control.c
 * @brief IPC control mode for OricForge IDE integration (sprint 35a)
 *
 * Implements the --control protocol described in include/control.h.
 * Reuses debugger.c primitives where possible.
 *
 * Sprint 92 (Epic 1): command handlers write to an abstract control_sink_t
 * instead of stdout directly, and the dispatch logic is factored into
 * control_dispatch() so the same handlers can back both the stdin/stdout IPC
 * protocol and a future HTTP API. The stdout path is byte-for-byte unchanged.
 */

#define _POSIX_C_SOURCE 200809L
#include "control.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "debugger.h"
#include "savestate.h"
#include "utils/logging.h"
#include "utils/symbols.h"
#include "io/via6522.h"
#include "audio/audio.h"
#include "io/microdisc.h"
#include "io/acia6551.h"
#include "io/loci.h"
#include "storage/disk.h"
#include "storage/sedoric.h"
#ifndef _WIN32
#include <sys/select.h>
#endif
#include <unistd.h>
#include <signal.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ─── response sink ────────────────────────────────────────────────
 * All protocol replies go through a control_sink_t. In stream mode the
 * bytes are written to the FILE and flushed immediately, so the IDE
 * observes traffic in real time (identical to the historical behaviour).
 * In buffer mode they accumulate in a growable, binary-safe buffer. */

void control_sink_init_stream(control_sink_t* s, FILE* fp) {
    s->fp = fp; s->buf = NULL; s->len = 0; s->cap = 0; s->error = false;
}

void control_sink_init_buffer(control_sink_t* s) {
    s->fp = NULL; s->buf = NULL; s->len = 0; s->cap = 0; s->error = false;
}

void control_sink_free(control_sink_t* s) {
    if (s && !s->fp) { free(s->buf); s->buf = NULL; s->len = 0; s->cap = 0; }
}

/* Grow a buffer-mode sink so it can hold @p extra more bytes plus a NUL. */
static void sink_ensure(control_sink_t* s, size_t extra) {
    if (s->fp) return;
    if (s->len + extra + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 256;
        while (ncap < s->len + extra + 1) ncap *= 2;
        char* nb = (char*)realloc(s->buf, ncap);
        if (!nb) { s->error = true; return; }
        s->buf = nb; s->cap = ncap;
    }
}

/* Binary-safe primitive: append @p len bytes of @p data to the sink. */
static void sink_write(control_sink_t* s, const void* data, size_t len) {
    if (!s || s->error || len == 0) return;
    if (s->fp) {
        if (fwrite(data, 1, len, s->fp) != len) s->error = true;
    } else {
        sink_ensure(s, len);
        if (s->error) return;
        memcpy(s->buf + s->len, data, len);
        s->len += len;
        s->buf[s->len] = '\0';
    }
}

static void sink_vprintf(control_sink_t* s, const char* fmt, va_list ap) {
    char tmp[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n < 0) { va_end(ap2); return; }
    if ((size_t)n < sizeof(tmp)) {
        sink_write(s, tmp, (size_t)n);
    } else {
        char* big = (char*)malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            sink_write(s, big, (size_t)n);
            free(big);
        }
    }
    va_end(ap2);
}

/* Append formatted text (no implicit newline, no flush). */
static void sink_printf(control_sink_t* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sink_vprintf(s, fmt, ap);
    va_end(ap);
}

/* Flush a stream-mode sink; no-op for buffer mode. */
static void sink_flush(control_sink_t* s) {
    if (s && s->fp) fflush(s->fp);
}

/* "OK" [ " " <fmt...> ] "\n", then flush — matches the legacy reply_ok(). */
static void sink_ok(control_sink_t* s, const char* fmt, ...) {
    sink_write(s, "OK", 2);
    if (fmt && *fmt) {
        sink_write(s, " ", 1);
        va_list ap;
        va_start(ap, fmt);
        sink_vprintf(s, fmt, ap);
        va_end(ap);
    }
    sink_write(s, "\n", 1);
    sink_flush(s);
}

/* "ERR " <fmt...> "\n", then flush — matches the legacy reply_err(). */
static void sink_err(control_sink_t* s, const char* fmt, ...) {
    sink_write(s, "ERR ", 4);
    va_list ap;
    va_start(ap, fmt);
    sink_vprintf(s, fmt, ap);
    va_end(ap);
    sink_write(s, "\n", 1);
    sink_flush(s);
}

/* ─── output helpers (events) ──────────────────────────────────────
 * Asynchronous events (EVT) are tied to the stdout IPC channel; they are
 * not part of a request/response and keep writing to stdout directly. */

static void emit_evt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("EVT ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

/* Sprint 35a freeze — non-blocking stdin check called from the main loop
 * once per frame while the CPU is running. Returns true if the client
 * sent `pause` and the loop should hand control back to the REPL.
 * Other commands during running are NOT queued: `quit` exits, anything
 * else is rejected with ERR busy. Trade-off: simpler semantics for the
 * IDE, no command races. */
bool control_poll_pause(emulator_t* emu) {
    if (!emu->control_mode) return false;
    /* Also surface a broken stdout to the main loop so we don't keep
     * running a session no one is listening to. */
    if (ferror(stdout)) { emu->running = false; return true; }
#ifdef _WIN32
    /* select() only works on sockets under Winsock — async-pause while
     * running is not available in the Windows v1 build (commands are
     * still processed at every stop/EVT boundary). */
    return false;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) return false;
    if (!FD_ISSET(STDIN_FILENO, &fds)) return false;

    char line[1024];
    if (!fgets(line, sizeof(line), stdin)) {
        emu->running = false;
        return true;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0) return false;

    control_sink_t s;
    control_sink_init_stream(&s, stdout);

    /* Strip first token for comparison. */
    char tok[16] = {0};
    sscanf(line, "%15s", tok);
    if (strcmp(tok, "pause") == 0) {
        sink_ok(&s, "pc=%04X cycles=%llu",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
        emu->control_async_pause_pending = true;
        return true;
    }
    if (strcmp(tok, "quit") == 0) {
        sink_ok(&s, "");
        emu->running = false;
        return true;
    }
    sink_err(&s, "busy: emulator running, only `pause`/`quit` allowed "
             "(received `%s`)", tok);
    return false;
#endif /* _WIN32 */
}

void control_emit_ready(emulator_t* emu) {
    /* Sprint 35c hardening — install SIGPIPE handler so a dead IDE
     * stdout pipe doesn't terminate us; we detect failed writes via
     * ferror(stdout) and shut down cleanly. Idempotent. */
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    emit_evt("ready pc=%04X cycles=%llu version=%s",
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles, EMU_VERSION);
    /* If the IDE has already closed its end before we got here, ferror
     * is set; surface it so the main loop exits instead of looping. */
    if (ferror(stdout)) emu->running = false;
}

void control_emit_stopped(emulator_t* emu, const char* reason) {
    emit_evt("stopped pc=%04X cycles=%llu reason=%s",
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles,
             reason ? reason : "unknown");
}

void control_emit_halt(emulator_t* emu, const char* reason) {
    emit_evt("halt pc=%04X cycles=%llu reason=%s",
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles,
             reason ? reason : "unknown");
}

/* ─── parsing helpers ──────────────────────────────────────────────
 * Accept hex with or without `$`/`0x` prefix, plus plain decimal when
 * unambiguous. The IDE side is well-defined, so we stay strict. */

static bool parse_hex(const char* s, uint32_t* out) {
    if (!s || !*s) return false;
    int base = 16;
    if (*s == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    else if (*s == '%') { s++; base = 2; }   /* binary literal */
    char* end = NULL;
    unsigned long v = strtoul(s, &end, base);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_u16(const char* s, uint16_t* out) {
    uint32_t v;
    if (!parse_hex(s, &v) || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

static bool parse_u8(const char* s, uint8_t* out) {
    uint32_t v;
    if (!parse_hex(s, &v) || v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}

/* ─── command handlers ─────────────────────────────────────────────
 * Each handler writes its reply to the sink; it no longer knows whether
 * the destination is stdout or an in-memory buffer. */

static void cmd_regs(emulator_t* emu, control_sink_t* s) {
    sink_ok(s, "A=%02X X=%02X Y=%02X SP=%02X P=%02X PC=%04X cycles=%llu",
            emu->cpu.A, emu->cpu.X, emu->cpu.Y, emu->cpu.SP, emu->cpu.P,
            emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
}

static void cmd_set(emulator_t* emu, control_sink_t* s,
                    const char* reg, const char* val, const char* tail) {
    if (!reg || !val) { sink_err(s, "set: usage `set <reg> <val>` or `set via <reg 0-15> <val>`"); return; }
    /* `set via <reg 0-15> <val>` — write a VIA 6522 register. Here `val`
     * carries the register index and `tail` the value. */
    if (strcasecmp(reg, "via") == 0) {
        uint32_t regn, vv;
        if (!tail || !parse_hex(val, &regn) || !parse_hex(tail, &vv) || regn > 15) {
            sink_err(s, "set: usage `set via <reg 0-15> <val>`");
            return;
        }
        via_write(&emu->via, (uint8_t)regn, (uint8_t)vv);
        sink_ok(s, "via=%u val=%02X", (unsigned)regn, (uint8_t)vv);
        return;
    }
    uint32_t v;
    if (!parse_hex(val, &v)) { sink_err(s, "set: bad value"); return; }
    /* Case-insensitive: A, X, Y, SP, P, PC. */
    if (strcasecmp(reg, "a")  == 0) emu->cpu.A  = (uint8_t)v;
    else if (strcasecmp(reg, "x")  == 0) emu->cpu.X  = (uint8_t)v;
    else if (strcasecmp(reg, "y")  == 0) emu->cpu.Y  = (uint8_t)v;
    else if (strcasecmp(reg, "sp") == 0) emu->cpu.SP = (uint8_t)v;
    else if (strcasecmp(reg, "p")  == 0) emu->cpu.P  = (uint8_t)v;
    else if (strcasecmp(reg, "pc") == 0) emu->cpu.PC = (uint16_t)v;
    else { sink_err(s, "set: unknown reg `%s`", reg); return; }
    sink_ok(s, "");
}

static void cmd_read(emulator_t* emu, control_sink_t* s,
                     const char* addr_s, const char* len_s, const char* bank_s) {
    uint16_t addr;
    uint32_t len;
    if (!parse_u16(addr_s, &addr) || !parse_hex(len_s, &len)) {
        sink_err(s, "read: usage `read <addr> <len> [cpu|ram|rom|overlay]`");
        return;
    }
    if (len > 4096) { sink_err(s, "read: len > 4096"); return; }
    peek_bank_t bank = PEEK_CPU;
    if (bank_s && *bank_s && !debugger_parse_bank(bank_s, &bank)) {
        sink_err(s, "read: bad bank `%s` (cpu|ram|rom|overlay)", bank_s);
        return;
    }
    /* Build the reply : "OK <hex bytes>". */
    sink_printf(s, "OK");
    for (uint32_t i = 0; i < len; i++) {
        sink_printf(s, " %02X", debugger_peek_bank(emu, (uint16_t)(addr + i), bank));
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

/* Sprint 35c — length-prefixed binary read. Up to 64 KB per call.
 * Wire format:
 *   client → `bread $XXXX <len>\n`
 *   server → `OK bread len=<len>\n`
 *   server → <len raw bytes>
 *   server → `\n`
 * The trailing newline lets a line-based client reader resync after
 * the binary chunk. The client must temporarily switch to raw-read
 * mode for the binary section (see phos_smoke_client.py::bread). */
static void cmd_bread(emulator_t* emu, control_sink_t* s,
                      const char* addr_s, const char* len_s) {
    uint16_t addr;
    uint32_t len;
    if (!parse_u16(addr_s, &addr) || !parse_hex(len_s, &len)) {
        sink_err(s, "bread: usage `bread <addr> <len>`");
        return;
    }
    if (len == 0 || len > 0x10000) {
        sink_err(s, "bread: len must be 1..65536");
        return;
    }
    /* Stage the buffer first, then emit the OK + binary in a single
     * flush, so a partial write can't interleave with another reply. */
    static uint8_t buf[0x10000];
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = memory_read(&emu->memory, (uint16_t)(addr + i));
    }
    sink_printf(s, "OK bread len=%u\n", len);
    sink_write(s, buf, len);
    sink_write(s, "\n", 1);
    sink_flush(s);
}

static void cmd_write(emulator_t* emu, control_sink_t* s, const char* addr_s,
                      const char* first_byte, char* rest_save) {
    uint16_t addr;
    if (!addr_s || !parse_u16(addr_s, &addr) || !first_byte) {
        sink_err(s, "write: usage `write <addr> <byte>...`");
        return;
    }
    uint8_t b;
    if (!parse_u8(first_byte, &b)) {
        sink_err(s, "write: bad byte at offset 0");
        return;
    }
    memory_write(&emu->memory, addr, b);
    int n = 1;
    char* tok;
    while ((tok = strtok_r(NULL, " \t", &rest_save)) != NULL) {
        if (!parse_u8(tok, &b)) {
            sink_err(s, "write: bad byte at offset %d", n);
            return;
        }
        memory_write(&emu->memory, (uint16_t)(addr + n), b);
        n++;
    }
    sink_ok(s, "count=%d", n);
}

static void cmd_break(emulator_t* emu, control_sink_t* s,
                      const char* addr_s, const char* cond) {
    uint16_t addr;
    if (!parse_u16(addr_s, &addr)) {
        sink_err(s, "break: usage `break <addr> [if <expr>]`");
        return;
    }
    if (cond && *cond) {
        int id = debugger_add_cond_breakpoint(&emu->debugger, emu, addr, cond);
        if (id == -2) { sink_err(s, "break: bad condition `%s`", cond); return; }
        if (id < 0)   { sink_err(s, "break: full or rejected"); return; }
        sink_ok(s, "id=%d addr=%04X cond=\"%s\"", id, addr,
                emu->debugger.breakpoints[id].cond_text);
        return;
    }
    int id = debugger_add_breakpoint(&emu->debugger, addr);
    if (id < 0) { sink_err(s, "break: full or rejected"); return; }
    sink_ok(s, "id=%d addr=%04X", id, addr);
}

static void cmd_unbreak(emulator_t* emu, control_sink_t* s, const char* id_s) {
    if (!id_s) { sink_err(s, "unbreak: usage `unbreak <id>`"); return; }
    int id = atoi(id_s);
    if (!debugger_remove_breakpoint(&emu->debugger, id)) {
        sink_err(s, "unbreak: invalid id");
        return;
    }
    sink_ok(s, "");
}

/* Sprint 35b — watchpoints. Sprint 97 — optional access mode w|r|a|c. */
static void cmd_watch(emulator_t* emu, control_sink_t* s,
                      const char* addr_s, const char* mode_s) {
    uint16_t addr;
    if (!parse_u16(addr_s, &addr)) {
        sink_err(s, "watch: usage `watch <addr> [w|r|a|c]`");
        return;
    }
    watch_mode_t mode = WATCH_WRITE;
    const char* mname = "write";
    if (mode_s && *mode_s) {
        switch (mode_s[0]) {
            case 'w': mode = WATCH_WRITE;  mname = "write";  break;
            case 'r': mode = WATCH_READ;   mname = "read";   break;
            case 'a': mode = WATCH_ACCESS; mname = "access"; break;
            case 'c': mode = WATCH_CHANGE; mname = "change"; break;
            default: sink_err(s, "watch: bad mode `%s` (w|r|a|c)", mode_s); return;
        }
    }
    int id = debugger_add_watchpoint_mode(&emu->debugger, addr, mode);
    if (id < 0) { sink_err(s, "watch: full or rejected"); return; }
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "id=%d addr=%04X mode=%s", id, addr, mname);
}

/* US 3 — access-flag map: per-byte r/w/x breakpoints over a region. */
static void cmd_watch_region(emulator_t* emu, control_sink_t* s,
                             const char* start_s, const char* end_s, const char* flags_s) {
    uint16_t start, end;
    if (!parse_u16(start_s, &start) || !parse_u16(end_s, &end)) {
        sink_err(s, "watch-region: usage `watch-region <start> <end> [rwx]`");
        return;
    }
    uint8_t flags = AMAP_R | AMAP_W;   /* default rw */
    if (flags_s && *flags_s && !debugger_amap_parse_flags(flags_s, &flags)) {
        sink_err(s, "watch-region: bad flags `%s` (subset of rwx)", flags_s);
        return;
    }
    uint32_t n = debugger_amap_set(start, end, flags);
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "flagged=%u start=%04X end=%04X flags=%c%c%c", n, start, end,
            (flags & AMAP_R) ? 'r' : '-', (flags & AMAP_W) ? 'w' : '-',
            (flags & AMAP_X) ? 'x' : '-');
}

static void cmd_watch_region_clear(emulator_t* emu, control_sink_t* s) {
    debugger_amap_clear();
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "");
}

static void cmd_watch_region_list(emulator_t* emu, control_sink_t* s) {
    (void)emu;
    sink_printf(s, "OK count=%u", debugger_amap_count());
    int shown = 0;
    uint32_t a = 0;
    while (a < 0x10000 && shown < 64) {
        uint8_t f = debugger_amap_get((uint16_t)a);
        if (!f) { a++; continue; }
        uint32_t start = a;
        while (a < 0x10000 && debugger_amap_get((uint16_t)a) == f) a++;
        sink_printf(s, " %04X-%04X:%c%c%c", (uint16_t)start, (uint16_t)(a - 1),
                    (f & AMAP_R) ? 'r' : '-', (f & AMAP_W) ? 'w' : '-',
                    (f & AMAP_X) ? 'x' : '-');
        shown++;
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

static void cmd_unwatch(emulator_t* emu, control_sink_t* s, const char* id_s) {
    if (!id_s) { sink_err(s, "unwatch: usage `unwatch <id>`"); return; }
    int id = atoi(id_s);
    if (!debugger_remove_watchpoint(&emu->debugger, id)) {
        sink_err(s, "unwatch: invalid id");
        return;
    }
    debugger_install_watchpoint_trace(&emu->debugger, emu);
    sink_ok(s, "");
}

static void cmd_watch_list(emulator_t* emu, control_sink_t* s) {
    debugger_t* dbg = &emu->debugger;
    sink_printf(s, "OK");
    for (int i = 0; i < dbg->num_watchpoints; i++) {
        static const char* mode_ch = "wrac";  /* write/read/access/change */
        sink_printf(s, " id=%d:addr=%04X:mode=%c", i,
                    dbg->watchpoints[i].addr, mode_ch[dbg->watchpoints[i].mode]);
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

/* Sprint 35b — raster-line breakpoints (PAL 0..311). */
static void cmd_raster(emulator_t* emu, control_sink_t* s, const char* line_s) {
    if (!line_s) { sink_err(s, "raster: usage `raster <line>`"); return; }
    int line = atoi(line_s);
    if (line < 0 || line >= PAL_LINES_PER_FRAME) {
        sink_err(s, "raster: line must be 0..%d", PAL_LINES_PER_FRAME - 1);
        return;
    }
    debugger_t* dbg = &emu->debugger;
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (dbg->raster_bps[i] < 0) { slot = i; break; }
    }
    if (slot < 0) { sink_err(s, "raster: all 8 slots used"); return; }
    dbg->raster_bps[slot] = (int16_t)line;
    dbg->num_raster_bps++;
    sink_ok(s, "id=%d line=%d", slot, line);
}

static void cmd_unraster(emulator_t* emu, control_sink_t* s, const char* id_s) {
    if (!id_s) { sink_err(s, "unraster: usage `unraster <id>`"); return; }
    int id = atoi(id_s);
    if (id < 0 || id >= 8 || emu->debugger.raster_bps[id] < 0) {
        sink_err(s, "unraster: invalid id");
        return;
    }
    emu->debugger.raster_bps[id] = -1;
    emu->debugger.num_raster_bps--;
    sink_ok(s, "");
}

/* Sprint 35b — runtime load helpers. They call the same primitives as
 * the CLI bootstrap path: file existence + size + memcpy into the right
 * slot. Errors return ERR with a short description. */
static bool load_file_into(const char* path, uint8_t** out_buf,
                           size_t* out_len, size_t max_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > max_len) { fclose(fp); return false; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(fp); return false; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (n != (size_t)sz) { free(buf); return false; }
    *out_buf = buf;
    *out_len = (size_t)sz;
    return true;
}

static void cmd_load_tap(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "load-tap: usage `load-tap <path>`"); return; }
    uint8_t* buf = NULL;
    size_t len = 0;
    if (!load_file_into(path, &buf, &len, 1 << 20)) {
        sink_err(s, "load-tap: cannot read `%s`", path);
        return;
    }
    if (emu->tapebuf) free(emu->tapebuf);
    emu->tapebuf = buf;
    emu->tapelen = (int)len;
    emu->tapeoffs = 0;
    emu->tape_loaded = true;
    emu->tape_path = strdup(path);
    sink_ok(s, "size=%zu", len);
}

/* Parse a drive selector: "A".."D", "a".."d" or "0".."3". -1 if invalid. */
static int control_drive_index(const char* s) {
    if (!s || !s[0] || s[1] != '\0') return -1;
    char c = s[0];
    if (c >= 'a' && c <= 'd') return c - 'a';
    if (c >= 'A' && c <= 'D') return c - 'A';
    if (c >= '0' && c <= '3') return c - '0';
    return -1;
}

/* Write a modified .dsk back to its source file, mirroring osd_writeback_drive()
 * in main.c. Opt-in via --disk-writeback; only when the drive is dirty and has a
 * known source path. Returns true if a write-back actually happened. */
static bool control_writeback_drive(emulator_t* emu, int drv) {
    if (!emu->disk_writeback || drv < 0 || drv >= MICRODISC_MAX_DRIVES) return false;
    if (!emu->microdisc.disk_dirty[drv] || !emu->disks[drv] || !emu->disk_paths[drv])
        return false;
    bool ok = sedoric_save(emu->disks[drv], emu->disk_paths[drv]);
    log_info("control: write-back drive %c -> %s (%s)", 'A' + drv,
             emu->disk_paths[drv], ok ? "OK" : "FAIL");
    emu->microdisc.disk_dirty[drv] = false;
    return ok;
}

/* load-disk <drive> <path> — hot-swap a .dsk into drive A-D. Mirrors the OSD
 * hot-load path: write back the outgoing disk (if dirty + --disk-writeback),
 * free it, then load and wire the new image. */
static void cmd_load_disk(emulator_t* emu, control_sink_t* s,
                          const char* drive_s, const char* path) {
    if (!drive_s || !path) {
        sink_err(s, "load-disk: usage `load-disk <drive A-D> <path>`");
        return;
    }
    if (!emu->has_microdisc) {
        sink_err(s, "load-disk: no microdisc (need --disk-rom)");
        return;
    }
    int drv = control_drive_index(drive_s);
    if (drv < 0) { sink_err(s, "load-disk: drive must be A-D"); return; }

    sedoric_disk_t* nd = sedoric_load(path);
    if (!nd) { sink_err(s, "load-disk: cannot read `%s`", path); return; }

    control_writeback_drive(emu, drv);
    if (emu->disks[drv]) sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = nd;
    emu->microdisc.disk_dirty[drv] = false;
    microdisc_set_disk(&emu->microdisc, (uint8_t)drv, nd->data, nd->size,
                       nd->tracks, nd->sectors);
    emu->disk_paths[drv] = strdup(path);
    if (drv == 0) emu->disk_path = emu->disk_paths[drv];
    log_info("control: disk %c <- %s", 'A' + drv, path);
    sink_ok(s, "drive=%c size=%u tracks=%u sectors=%u", 'A' + drv,
            nd->size, nd->tracks, nd->sectors);
}

/* eject-disk <drive> — empty drive A-D, writing back first if dirty. */
static void cmd_eject_disk(emulator_t* emu, control_sink_t* s,
                           const char* drive_s) {
    if (!emu->has_microdisc) { sink_err(s, "eject-disk: no microdisc"); return; }
    int drv = control_drive_index(drive_s);
    if (drv < 0) { sink_err(s, "eject-disk: usage `eject-disk <drive A-D>`"); return; }
    if (!emu->disks[drv]) { sink_err(s, "eject-disk: drive %c already empty", 'A' + drv); return; }

    bool wb = control_writeback_drive(emu, drv);
    sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = NULL;
    emu->disk_paths[drv] = NULL;
    microdisc_set_disk(&emu->microdisc, (uint8_t)drv, NULL, 0, 0, 0);
    if (drv == 0) emu->disk_path = NULL;
    log_info("control: drive %c ejected", 'A' + drv);
    sink_ok(s, "drive=%c ejected writeback=%d", 'A' + drv, wb ? 1 : 0);
}

/* loci-button [long] — LOCI Action button, warm press + release. Same
 * path as F8 in the GUI: session snapshot, IRQ trap, then boot the menu
 * ROM (short) or the test108k diagnostic ROM ("long", ≥ 2 s hold). */
static void cmd_loci_button(emulator_t* emu, control_sink_t* s, const char* mode) {
    if (!emu->has_loci) {
        sink_err(s, "loci-button: LOCI not enabled (--loci)");
        return;
    }
    bool longp = (mode && strcmp(mode, "long") == 0);
    emu->loci_button_long = longp;
    loci_action_button_short(&emu->loci);
    loci_action_button_release(&emu->loci);
    sink_ok(s, "action-button pulsed%s", longp ? " (long)" : "");
}

/* eject-tape — unload the cassette and free its buffer. */
static void cmd_eject_tape(emulator_t* emu, control_sink_t* s) {
    if (!emu->tape_loaded && !emu->tapebuf) {
        sink_err(s, "eject-tape: no tape loaded");
        return;
    }
    if (emu->tapebuf) { free(emu->tapebuf); emu->tapebuf = NULL; }
    emu->tapelen = 0;
    emu->tapeoffs = 0;
    emu->tape_loaded = false;
    emu->tape_path = NULL;
    log_info("control: tape ejected");
    sink_ok(s, "ejected");
}

static void cmd_load_rom(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "load-rom: usage `load-rom <path>`"); return; }
    uint8_t* buf = NULL;
    size_t len = 0;
    /* Cap at 16 KB — typical Oric BASIC ROM. */
    if (!load_file_into(path, &buf, &len, 16 * 1024)) {
        sink_err(s, "load-rom: cannot read `%s`", path);
        return;
    }
    if (len != 16 * 1024) {
        free(buf);
        sink_err(s, "load-rom: expected 16384 bytes, got %zu", len);
        return;
    }
    memcpy(emu->memory.rom, buf, len);
    free(buf);
    cpu_reset(&emu->cpu);
    sink_ok(s, "size=%zu pc=%04X", len, emu->cpu.PC);
}

static void cmd_load_sym(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "load-sym: usage `load-sym <path>`"); return; }
    int n = symbol_table_load(&emu->symbols, path);
    if (n < 0) { sink_err(s, "load-sym: parse failed"); return; }
    sink_ok(s, "count=%d total=%d", n, emu->symbols.count);
}

/* Sprint 35b — disassemble N instructions starting at addr. */
static void cmd_disasm(emulator_t* emu, control_sink_t* s,
                       const char* addr_s, const char* n_s) {
    uint16_t addr;
    uint32_t n;
    if (!parse_u16(addr_s, &addr) || !parse_hex(n_s, &n)) {
        sink_err(s, "disasm: usage `disasm <addr> <n>`");
        return;
    }
    if (n == 0 || n > 64) { sink_err(s, "disasm: n must be 1..64"); return; }
    /* One reply line per instruction. */
    for (uint32_t i = 0; i < n; i++) {
        char buf[64];
        int bytes = cpu_disassemble(&emu->cpu, addr, buf, sizeof(buf));
        const char* sym = symbol_lookup(&emu->symbols, addr);
        sink_printf(s, "OK addr=%04X bytes=%d disasm=\"%s\"",
                    addr, bytes, buf);
        if (sym) sink_printf(s, " label=%s", sym);
        sink_printf(s, "\n");
        addr = (uint16_t)(addr + bytes);
    }
    sink_flush(s);
}

static void cmd_break_list(emulator_t* emu, control_sink_t* s) {
    debugger_t* dbg = &emu->debugger;
    sink_printf(s, "OK");
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        sink_printf(s, " id=%d:addr=%04X", i, dbg->breakpoints[i].addr);
        if (dbg->breakpoints[i].has_cond)
            sink_printf(s, ":cond=\"%s\"", dbg->breakpoints[i].cond_text);
    }
    sink_printf(s, "\n");
    sink_flush(s);
}

/* Sprint 97 — iterative memory search (cheat-finder), mirrors the REPL `hunt`.
 * `hunt` (no op) seeds; op ∈ {eq <v>, same, changed, up, down, list, clear}. */
static void cmd_hunt(emulator_t* emu, control_sink_t* s,
                     const char* op, const char* val_s) {
    if (!op || !*op) {
        debugger_hunt_start(emu);
        sink_ok(s, "candidates=%u", debugger_hunt_count());
        return;
    }
    if (strcasecmp(op, "clear") == 0) { debugger_hunt_clear(); sink_ok(s, ""); return; }
    if (strcasecmp(op, "list") == 0) {
        if (!debugger_hunt_active()) { sink_err(s, "hunt: not active"); return; }
        uint16_t addrs[64]; uint8_t vals[64];
        uint32_t n = debugger_hunt_list(emu, addrs, 64, vals);
        sink_printf(s, "OK count=%u", debugger_hunt_count());
        for (uint32_t i = 0; i < n; i++)
            sink_printf(s, " %04X=%02X", addrs[i], vals[i]);
        sink_printf(s, "\n");
        sink_flush(s);
        return;
    }
    if (!debugger_hunt_active()) { sink_err(s, "hunt: not active (send `hunt` first)"); return; }
    hunt_pred_t pred; uint8_t val = 0;
    if (strcasecmp(op, "eq") == 0) {
        uint32_t v;
        if (!val_s || !parse_hex(val_s, &v) || v > 0xFF) { sink_err(s, "hunt: eq needs a byte"); return; }
        pred = HUNT_EQ; val = (uint8_t)v;
    } else if (strcasecmp(op, "same") == 0)    pred = HUNT_UNCHANGED;
    else if (strcasecmp(op, "changed") == 0)   pred = HUNT_CHANGED;
    else if (strcasecmp(op, "up") == 0)        pred = HUNT_GT;
    else if (strcasecmp(op, "down") == 0)      pred = HUNT_LT;
    else { sink_err(s, "hunt: op ∈ {eq,same,changed,up,down,list,clear}"); return; }
    uint32_t k = debugger_hunt_refine(emu, pred, val);
    sink_ok(s, "candidates=%u", k);
}

/* Sprint 97 — memory ⇄ file region + full save-state from the protocol. */
static void cmd_save_mem(emulator_t* emu, control_sink_t* s,
                         const char* path, const char* addr_s, const char* len_s) {
    uint16_t addr; uint32_t len;
    if (!path || !parse_u16(addr_s, &addr) || !parse_hex(len_s, &len)) {
        sink_err(s, "save-mem: usage `save-mem <file> <addr> <len>`");
        return;
    }
    if (debugger_save_region(emu, path, addr, len)) sink_ok(s, "wrote=%u addr=%04X", len, addr);
    else sink_err(s, "save-mem: write failed");
}

static void cmd_load_mem(emulator_t* emu, control_sink_t* s,
                         const char* path, const char* addr_s) {
    uint16_t addr;
    if (!path || !parse_u16(addr_s, &addr)) {
        sink_err(s, "load-mem: usage `load-mem <file> <addr>`");
        return;
    }
    long n = debugger_load_region(emu, path, addr);
    if (n < 0) sink_err(s, "load-mem: read failed");
    else sink_ok(s, "loaded=%ld addr=%04X", n, addr);
}

static void cmd_state_save(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "state-save: usage `state-save <file>`"); return; }
    if (savestate_save(emu, path)) sink_ok(s, "");
    else sink_err(s, "state-save: failed");
}

static void cmd_state_load(emulator_t* emu, control_sink_t* s, const char* path) {
    if (!path) { sink_err(s, "state-load: usage `state-load <file>`"); return; }
    if (savestate_load(emu, path)) sink_ok(s, "pc=%04X", emu->cpu.PC);
    else sink_err(s, "state-load: failed");
}

/* Epic 6 / US 1 — conditional CPU tracing. `sub` is the subcommand and `rest`
 * the raw remainder (a trace spec, or a filename for `save`). */
static void cmd_trace(emulator_t* emu, control_sink_t* s,
                      const char* sub, const char* rest) {
    cpu_trace_t* t = &emu->trace;
    if (!sub || !*sub || strcasecmp(sub, "status") == 0) {
        sink_ok(s, "active=%d armed=%d count=%llu ring=%u/%u stop_hit=%d",
                (int)t->active, (int)t->armed, (unsigned long long)t->count,
                trace_ring_count(t), t->ring_cap, (int)t->stop_hit);
    } else if (strcasecmp(sub, "start") == 0) {
        trace_start_t sc; uint16_t spc; trace_stop_t stc; uint16_t sa;
        uint64_t scy; uint32_t ring; bool sym;
        if (!trace_parse_spec(rest, &sc, &spc, &stc, &sa, &scy, &ring, &sym)) {
            sink_err(s, "trace: bad spec (now|pc:HEX stop:cycle:N|brk|write:HEX|read:HEX ring:N sym)");
            return;
        }
        trace_set_symbols(t, &emu->symbols);
        trace_arm(t, sc, spc, stc, sa, scy, ring, sym);
        trace_install_mem_hook(t, &emu->memory);
        sink_ok(s, "armed active=%d ring=%u", (int)t->active, t->ring_cap);
    } else if (strcasecmp(sub, "stop") == 0) {
        trace_stop(t);
        sink_ok(s, "count=%llu ring=%u", (unsigned long long)t->count, trace_ring_count(t));
    } else if (strcasecmp(sub, "save") == 0) {
        if (!rest || !*rest) { sink_err(s, "trace: usage `trace save <file>`"); return; }
        if (trace_save_ring(t, rest)) sink_ok(s, "saved=%u", trace_ring_count(t));
        else sink_err(s, "trace: nothing to save / write failed");
    } else if (strcasecmp(sub, "off") == 0) {
        trace_reset(t);
        trace_install_mem_hook(t, &emu->memory);   /* drops the memory hook */
        sink_ok(s, "");
    } else {
        sink_err(s, "trace: unknown subcommand `%s`", sub);
    }
}

static void cmd_reset(emulator_t* emu, control_sink_t* s) {
    cpu_reset(&emu->cpu);
    sink_ok(s, "pc=%04X", emu->cpu.PC);
}

/* Sprint 35a freeze — protocol version + capability list. Bumped whenever
 * an existing command or event changes shape (additive `caps=` extensions
 * do NOT bump the version). */
#define CONTROL_PROTO_VERSION 1
#define CONTROL_PROTO_CAPS    "step-out,peek,hello,async-pause,watch,raster,load-tap,load-rom,load-sym,disasm,bread,load-disk,eject-disk,eject-tape,loci-button,keys,watch-mode,break-cond,hunt,save-mem,load-mem,state-save,state-load,set-via,bin-literal,mem-bank,trace-cond,access-map"

static void cmd_hello(control_sink_t* s, const char* arg1, const char* arg2) {
    (void)arg1; (void)arg2;
    sink_ok(s, "server=phosphoric/%s proto=%d caps=%s",
            EMU_VERSION, CONTROL_PROTO_VERSION, CONTROL_PROTO_CAPS);
}

/* Sprint 35a freeze — `peek <subsystem>` exposes the per-device REPL
 * commands (via/psg/disk/acia/tape/loci) in a single-line key=value
 * format so the IDE can populate its inspectors without parsing
 * human-friendly output. Each branch emits one line. */
static void cmd_peek(emulator_t* emu, control_sink_t* s, const char* sub) {
    if (!sub) { sink_err(s, "peek: usage `peek <subsystem>`"); return; }
    if (strcmp(sub, "via") == 0) {
        via6522_t* v = &emu->via;
        sink_ok(s, "ora=%02X orb=%02X ddra=%02X ddrb=%02X "
                "t1c=%04X t1l=%04X t2c=%04X t2l=%02X "
                "acr=%02X pcr=%02X ifr=%02X ier=%02X sr=%02X "
                "t1_run=%d t2_run=%d",
                v->ora, v->orb, v->ddra, v->ddrb,
                v->t1_counter, v->t1_latch, v->t2_counter, v->t2_latch,
                v->acr, v->pcr, v->ifr, v->ier, v->sr,
                v->t1_running ? 1 : 0, v->t2_running ? 1 : 0);
    }
    else if (strcmp(sub, "psg") == 0) {
        ay3891x_t* p = &emu->psg;
        sink_printf(s, "OK");
        for (int i = 0; i < 14; i++) sink_printf(s, " r%d=%02X", i, p->registers[i]);
        sink_printf(s, " env_period=%u env_shape=%u env_step=%u env_vol=%u",
                    p->env_period, p->env_shape, p->env_step, p->env_volume);
        sink_printf(s, "\n");
        sink_flush(s);
    }
    else if (strcmp(sub, "disk") == 0 || strcmp(sub, "fdc") == 0) {
        if (!emu->has_microdisc) { sink_err(s, "disk: microdisc inactive"); return; }
        microdisc_t* md = &emu->microdisc;
        fdc_t* f = &md->fdc;
        sink_ok(s, "ctrl=%02X intrq=%d drq=%d diskrom=%d romdis=%d intena=%d "
                "drive=%d side=%d cmd=%02X status=%02X trk=%02X sec=%02X "
                "data=%02X dir=%d c_trk=%02X c_sec=%02X cur_off=%04X "
                "drives_mounted=%d%d%d%d",
                md->status,
                md->intrq == 0x00 ? 1 : 0, md->drq == 0x00 ? 1 : 0,
                md->diskrom, md->romdis, md->intena, md->drive, md->side,
                f->command, f->status, f->track, f->sector, f->data,
                f->direction, f->c_track, f->c_sector, f->cur_offset,
                md->disk_data[0] != NULL, md->disk_data[1] != NULL,
                md->disk_data[2] != NULL, md->disk_data[3] != NULL);
    }
    else if (strcmp(sub, "acia") == 0 || strcmp(sub, "serial") == 0) {
        acia6551_t* a = &emu->acia;
        sink_ok(s, "tdr=%02X rdr=%02X status=%02X cmd=%02X ctrl=%02X "
                "framebits=%u baud=%u v23=%d tx_pending=%d rx_full=%d "
                "irq_line=%d dcd=%d dsr=%d cts=%d rx_fifo_count=%d "
                "rx_fifo_size=%d",
                a->tdr, a->rdr, a->status, a->command, a->control,
                a->framebits, a->baud_rate, a->v23_mode ? 1 : 0,
                a->tx_pending ? 1 : 0, a->rx_full ? 1 : 0,
                a->irq_line ? 1 : 0, a->dcd ? 1 : 0, a->dsr ? 1 : 0,
                a->cts ? 1 : 0, a->rx_fifo_count, a->rx_fifo_size);
    }
    else if (strcmp(sub, "tape") == 0 || strcmp(sub, "cassette") == 0) {
        sink_ok(s, "loaded=%d pos=%d len=%d sync_loop=%d cload_active=%d "
                "fastload_pending=%d csave_active=%d",
                emu->tape_loaded ? 1 : 0, emu->tapeoffs, emu->tapelen,
                emu->tape_syncstack >= 0 ? 1 : 0,
                emu->tape_readbyte_active ? 1 : 0,
                emu->fastload_pending ? 1 : 0,
                emu->csave_file != NULL ? 1 : 0);
    }
    else if (strcmp(sub, "loci") == 0) {
        if (!emu->has_loci) { sink_err(s, "loci: inactive"); return; }
        loci_t* l = &emu->loci;
        uint16_t err = (uint16_t)l->regs[LOCI_REG_API_ERRNO_LO]
                     | ((uint16_t)l->regs[LOCI_REG_API_ERRNO_HI] << 8);
        int fd_n = 0, dir_n = 0, mnt_n = 0;
        for (int i = 0; i < LOCI_FD_MAX; i++) if (l->fd_kind[i]) fd_n++;
        for (int i = 0; i < LOCI_DIR_MAX; i++) if (l->dir_kind[i]) dir_n++;
        for (int i = 0; i < LOCI_MNT_MAX; i++) if (l->mnt_mounted[i]) mnt_n++;
        uint64_t total = 0;
        for (int i = 0; i < 256; i++) total += l->op_count[i];
        sink_ok(s, "enabled=%d active_op=%02X errno=%u busy=%02X "
                "ops_total=%llu xstack_used=%u/%d "
                "fds_open=%d dirs_open=%d mounts=%d "
                "tap_pos=%u tap_size=%u dsk_selected=%d boot_settings=%02X "
                "sdimg=%d",
                l->enabled ? 1 : 0, l->active_op, err, l->regs[LOCI_REG_BUSY],
                (unsigned long long)total,
                LOCI_XSTACK_SIZE - l->xstack_ptr, LOCI_XSTACK_SIZE,
                fd_n, dir_n, mnt_n,
                l->tap_counter, l->tap_size,
                l->dsk_selected, l->boot_settings,
                l->sdimg ? 1 : 0);
    }
    else {
        sink_err(s, "peek: unknown subsystem `%s` "
                 "(via|psg|disk|acia|tape|loci)", sub);
    }
}

/* keys <text> — queue keystrokes for the main loop to inject (sprint 95).
 * Escapes: \n / \r → RETURN, \t → TAB, \e → ESC, \\ → backslash; every other
 * byte is literal (spaces preserved — only the command word was stripped).
 * Appends to the emulator's growable injection buffer; the main loop presses
 * one key every few frames. Runs on the emulator thread (queue drain), so the
 * shared buffer needs no lock. */
static void cmd_keys(emulator_t* emu, control_sink_t* s, const char* text) {
    if (!text) { sink_err(s, "keys: usage `keys <text>`"); return; }
    while (*text == ' ' || *text == '\t') text++;   /* skip leading blanks */
    if (!*text) { sink_err(s, "keys: empty text"); return; }

    size_t added = 0;
    for (const char* p = text; *p; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': case 'r': c = '\n'; break;   /* RETURN */
                case 't': c = '\t'; break;
                case 'e': c = (char)0x1B; break;       /* ESC */
                case '\\': c = '\\'; break;
                default: c = *p; break;                /* literal */
            }
        }
        if (emu->kbd_inject_len + 1 > emu->kbd_inject_cap) {
            size_t ncap = emu->kbd_inject_cap ? emu->kbd_inject_cap * 2 : 64;
            char* nb = (char*)realloc(emu->kbd_inject_buf, ncap);
            if (!nb) { sink_err(s, "keys: out of memory"); return; }
            emu->kbd_inject_buf = nb;
            emu->kbd_inject_cap = ncap;
        }
        emu->kbd_inject_buf[emu->kbd_inject_len++] = c;
        added++;
    }
    sink_ok(s, "queued=%zu pending=%zu", added,
            emu->kbd_inject_len - emu->kbd_inject_pos);
}

/* ─── dispatch ─────────────────────────────────────────────────────
 * Parse one command line (mutated in place by strtok_r) and execute it
 * into @p s. Resume/quit commands set the debugger flags and are signalled
 * to the caller through the return value; synchronous commands emit their
 * reply inline and return CONTROL_CONTINUE. */
control_result_t control_dispatch(emulator_t* emu, control_sink_t* s,
                                  char* line) {
    /* Strip trailing newline(s). */
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0) return CONTROL_CONTINUE;   /* blank line, ignore */

    /* First token = command. */
    char* save;
    char* cmd = strtok_r(line, " \t", &save);
    if (!cmd) return CONTROL_CONTINUE;

    /* `keys` consumes the raw remainder (spaces preserved), so handle it here
     * before the argument tokenizer chops the rest of the line into words. */
    if (strcmp(cmd, "keys") == 0) {
        cmd_keys(emu, s, save ? save : "");
        return CONTROL_CONTINUE;
    }

    /* `trace` needs its subcommand + raw remainder (a spec or filename), so it
     * is handled before the argument tokenizer chops the rest into words. */
    if (strcmp(cmd, "trace") == 0) {
        char* sub = strtok_r(NULL, " \t", &save);
        cmd_trace(emu, s, sub, save ? save : "");
        return CONTROL_CONTINUE;
    }

    char* arg1 = strtok_r(NULL, " \t", &save);
    char* arg2 = strtok_r(NULL, " \t", &save);

    if (strcmp(cmd, "hello") == 0) {
        cmd_hello(s, arg1, arg2);
    }
    else if (strcmp(cmd, "peek") == 0) {
        cmd_peek(emu, s, arg1);
    }
    else if (strcmp(cmd, "regs") == 0) {
        cmd_regs(emu, s);
    }
    else if (strcmp(cmd, "set") == 0) {
        cmd_set(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "read") == 0) {
        cmd_read(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "bread") == 0) {
        cmd_bread(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "write") == 0) {
        cmd_write(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "break") == 0) {
        cmd_break(emu, s, arg1,
                  (arg2 && strcasecmp(arg2, "if") == 0) ? save : NULL);
    }
    else if (strcmp(cmd, "unbreak") == 0) {
        cmd_unbreak(emu, s, arg1);
    }
    else if (strcmp(cmd, "break-list") == 0) {
        cmd_break_list(emu, s);
    }
    else if (strcmp(cmd, "watch") == 0) {
        cmd_watch(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "hunt") == 0) {
        cmd_hunt(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "save-mem") == 0) {
        char* a3 = strtok_r(NULL, " \t", &save);
        cmd_save_mem(emu, s, arg1, arg2, a3);
    }
    else if (strcmp(cmd, "load-mem") == 0) {
        cmd_load_mem(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "state-save") == 0) {
        cmd_state_save(emu, s, arg1);
    }
    else if (strcmp(cmd, "state-load") == 0) {
        cmd_state_load(emu, s, arg1);
    }
    else if (strcmp(cmd, "unwatch") == 0) {
        cmd_unwatch(emu, s, arg1);
    }
    else if (strcmp(cmd, "watch-list") == 0) {
        cmd_watch_list(emu, s);
    }
    else if (strcmp(cmd, "watch-region") == 0) {
        cmd_watch_region(emu, s, arg1, arg2, save);
    }
    else if (strcmp(cmd, "watch-region-clear") == 0) {
        cmd_watch_region_clear(emu, s);
    }
    else if (strcmp(cmd, "watch-region-list") == 0) {
        cmd_watch_region_list(emu, s);
    }
    else if (strcmp(cmd, "raster") == 0) {
        cmd_raster(emu, s, arg1);
    }
    else if (strcmp(cmd, "unraster") == 0) {
        cmd_unraster(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-tap") == 0) {
        cmd_load_tap(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-disk") == 0) {
        cmd_load_disk(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "eject-disk") == 0) {
        cmd_eject_disk(emu, s, arg1);
    }
    else if (strcmp(cmd, "eject-tape") == 0) {
        cmd_eject_tape(emu, s);
    }
    else if (strcmp(cmd, "loci-button") == 0) {
        cmd_loci_button(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-rom") == 0) {
        cmd_load_rom(emu, s, arg1);
    }
    else if (strcmp(cmd, "load-sym") == 0) {
        cmd_load_sym(emu, s, arg1);
    }
    else if (strcmp(cmd, "disasm") == 0) {
        cmd_disasm(emu, s, arg1, arg2);
    }
    else if (strcmp(cmd, "reset") == 0) {
        cmd_reset(emu, s);
    }
    else if (strcmp(cmd, "pause") == 0) {
        /* The REPL is only re-entered when execution is already
         * stopped, so `pause` is informational. */
        sink_ok(s, "pc=%04X cycles=%llu",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
    }
    else if (strcmp(cmd, "step") == 0) {
        emu->debugger.step_mode = true;
        emu->debugger.active = false;
        sink_ok(s, "");
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "next") == 0) {
        uint8_t opc = memory_read(&emu->memory, emu->cpu.PC);
        if (opc == 0x20) {
            emu->debugger.temp_breakpoint = (uint16_t)(emu->cpu.PC + 3);
            emu->debugger.has_temp_breakpoint = true;
            emu->debugger.step_mode = false;
        } else {
            emu->debugger.step_mode = true;
        }
        emu->debugger.active = false;
        sink_ok(s, "");
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "step-out") == 0) {
        /* Sprint 35a freeze-time addition : peek the return address
         * from the current stack frame (push order : hi first then lo,
         * so JSR stores PC-1 with hi at $0100+SP+2 and lo at SP+1).
         * RTS adds +1 to land on the instruction after JSR. */
        uint16_t sp = (uint16_t)(0x0100 + emu->cpu.SP);
        uint8_t lo = memory_read(&emu->memory, (uint16_t)(sp + 1));
        uint8_t hi = memory_read(&emu->memory, (uint16_t)(sp + 2));
        uint16_t ret = (uint16_t)(((uint16_t)hi << 8) | lo) + 1;
        emu->debugger.temp_breakpoint = ret;
        emu->debugger.has_temp_breakpoint = true;
        emu->debugger.step_mode = false;
        emu->debugger.active = false;
        sink_ok(s, "ret=%04X", ret);
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "continue") == 0) {
        emu->debugger.step_mode = false;
        emu->debugger.active = false;
        sink_ok(s, "");
        return CONTROL_RESUME;
    }
    else if (strcmp(cmd, "quit") == 0) {
        sink_ok(s, "");
        return CONTROL_QUIT;
    }
    else {
        sink_err(s, "unknown command `%s`", cmd);
    }
    return CONTROL_CONTINUE;
}

/* ─── main REPL loop ──────────────────────────────────────────── */

void control_repl(emulator_t* emu) {
    control_sink_t s;
    control_sink_init_stream(&s, stdout);
    char line[1024];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        control_result_t r = control_dispatch(emu, &s, line);
        if (r == CONTROL_RESUME) return;
        if (r == CONTROL_QUIT) { emu->running = false; return; }
    }
    /* EOF on stdin — treat as quit. */
    emu->running = false;
}
