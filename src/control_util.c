/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file control_util.c
 * @brief Commandes --control : tampons de réponse, événements, analyse des arguments
 * @author bmarty <bmarty@mailo.com>
 *
 * Découpé de src/control.c (sprint F du plan d'architecture), sans changement
 * de comportement ; fonctions partagées : include/control_internal.h.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE  /* macOS: _POSIX_C_SOURCE masque les extensions BSD (MSG_DONTWAIT...) */
#endif
#include "control.h"
#include "io/loci_emu.h"
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
#include "io/loci_internal.h"   /* loci_dsk_open / loci_dsk_close */
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

#include "control_internal.h"
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
void sink_ensure(control_sink_t* s, size_t extra) {
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
void sink_write(control_sink_t* s, const void* data, size_t len) {
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

void sink_vprintf(control_sink_t* s, const char* fmt, va_list ap) {
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
void sink_printf(control_sink_t* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sink_vprintf(s, fmt, ap);
    va_end(ap);
}

/* Flush a stream-mode sink; no-op for buffer mode. */
void sink_flush(control_sink_t* s) {
    if (s && s->fp) fflush(s->fp);
}

/* "OK" [ " " <fmt...> ] "\n", then flush — matches the legacy reply_ok(). */
void sink_ok(control_sink_t* s, const char* fmt, ...) {
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
void sink_err(control_sink_t* s, const char* fmt, ...) {
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

void emit_evt(const char* fmt, ...) {
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

bool ctl_parse_hex(const char* s, uint32_t* out) {
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

bool ctl_parse_u16(const char* s, uint16_t* out) {
    uint32_t v;
    if (!ctl_parse_hex(s, &v) || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

bool ctl_parse_u8(const char* s, uint8_t* out) {
    uint32_t v;
    if (!ctl_parse_hex(s, &v) || v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}
