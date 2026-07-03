/**
 * @file serial_backend_win.c
 * @brief Serial backends — Windows (MinGW-w64) variant, Sprint 89
 * @author bmarty <bmarty@mailo.com>
 *
 * Windows v1 scope: the pure-stdio backends (loopback, file) are fully
 * functional; the POSIX transports (tcp, pty, modem, com, digitelec,
 * picowifi) and host MIDI/SMF return NULL with a clear message. The
 * full serial_backend.h symbol surface is provided so main.c, dtl2000
 * and mageco link unchanged.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "io/serial_backend.h"
#include "utils/logging.h"

/* ── loopback: TX → circular buffer → RX ─────────────────────────── */

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
    if (self->state.loopback.count >= SERIAL_LOOPBACK_BUFSZ) return false;
    self->state.loopback.buf[self->state.loopback.head] = byte;
    self->state.loopback.head = (self->state.loopback.head + 1) % SERIAL_LOOPBACK_BUFSZ;
    self->state.loopback.count++;
    return true;
}

static bool loopback_recv(serial_backend_t* self, uint8_t* byte)
{
    if (self->state.loopback.count <= 0) return false;
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

/* ── file: in_path replayed as RX, TX captured to out_path ───────── */

static bool file_open(serial_backend_t* self)
{
    self->state.file.peeked = -1;
    self->state.file.in = NULL;
    self->state.file.out = NULL;
    if (self->state.file.in_path[0]) {
        FILE* f = fopen(self->state.file.in_path, "rb");
        if (!f) {
            log_error("Serial file: cannot open input '%s'", self->state.file.in_path);
            return false;
        }
        self->state.file.in = f;
    }
    if (self->state.file.out_path[0]) {
        FILE* f = fopen(self->state.file.out_path, "wb");
        if (!f) {
            log_error("Serial file: cannot open output '%s'", self->state.file.out_path);
            if (self->state.file.in) fclose((FILE*)self->state.file.in);
            self->state.file.in = NULL;
            return false;
        }
        self->state.file.out = f;
    }
    log_info("Serial file backend opened (in=%s out=%s)",
             self->state.file.in_path[0] ? self->state.file.in_path : "-",
             self->state.file.out_path[0] ? self->state.file.out_path : "-");
    return true;
}

static void file_close(serial_backend_t* self)
{
    if (self->state.file.in)  { fclose((FILE*)self->state.file.in);  self->state.file.in = NULL; }
    if (self->state.file.out) { fclose((FILE*)self->state.file.out); self->state.file.out = NULL; }
}

static bool file_send(serial_backend_t* self, uint8_t byte)
{
    if (self->state.file.out) {
        fputc(byte, (FILE*)self->state.file.out);
        fflush((FILE*)self->state.file.out);
    }
    return true;   /* TX always accepted (discarded without capture file) */
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
    self->state.file.peeked = c;   /* one-byte lookahead → accurate poll */
    return true;
}

static bool file_connected(serial_backend_t* self)
{
    (void)self;
    return true;
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
    if (in_path)
        strncpy(b->state.file.in_path, in_path, sizeof(b->state.file.in_path) - 1);
    if (out_path)
        strncpy(b->state.file.out_path, out_path, sizeof(b->state.file.out_path) - 1);
    b->state.file.peeked = -1;
    return b;
}

/* ── POSIX transports: not available in the Windows v1 build ─────── */

static serial_backend_t* win_unavailable(const char* what)
{
    log_error("Serial %s: non disponible dans le build Windows v1 "
              "(transports POSIX) — utilisez loopback ou file:, ou la "
              "version Linux/WSL2 pour tcp/pty/modem/com/picowifi", what);
    return NULL;
}

serial_backend_t* serial_backend_tcp_create(const char* host, uint16_t port)
{ (void)host; (void)port; return win_unavailable("tcp"); }

serial_backend_t* serial_backend_pty_create(void)
{ return win_unavailable("pty"); }

serial_backend_t* serial_backend_modem_create(const char* host, uint16_t port, bool listen_mode)
{ (void)host; (void)port; (void)listen_mode; return win_unavailable("modem"); }

serial_backend_t* serial_backend_com_create(const char* config)
{ (void)config; return win_unavailable("com"); }

serial_backend_t* serial_backend_digitelec_create(const char* host, uint16_t port, void* acia)
{ (void)host; (void)port; (void)acia; return win_unavailable("digitelec"); }

serial_backend_t* serial_backend_picowifi_create(const char* ssid, const char* pass)
{ (void)ssid; (void)pass; return win_unavailable("picowifi"); }

serial_backend_t* serial_backend_midi_create(const char* target)
{ (void)target; return win_unavailable("midi"); }

serial_backend_t* serial_backend_smf_create(const char* path, bool loop)
{ (void)path; (void)loop; return win_unavailable("smf"); }

/* ── destroy ─────────────────────────────────────────────────────── */

void serial_backend_destroy(serial_backend_t* backend)
{
    if (!backend) return;
    if (backend->close) backend->close(backend);
    free(backend);
}
