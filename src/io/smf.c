/**
 * @file smf.c
 * @brief Standard MIDI File parser — see smf.h
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-23
 *
 * Pure, deterministic SMF → timed wire-event parsing. Tracks are parsed into a
 * flat record list (absolute ticks), merged/sorted onto one timeline, then the
 * tempo map is integrated to assign microsecond timestamps. No I/O timing here.
 */

#include "io/smf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMF_DEFAULT_TEMPO_US  500000u   /* 120 BPM = 500000 µs / quarter note */

/* ── Intermediate record collected while parsing a track ───────────────── */
typedef struct {
    uint32_t tick;       /* absolute tick from start of track */
    uint32_t seq;        /* global parse order (stable-sort tiebreak) */
    bool     is_tempo;   /* true → tempo change (not a wire event) */
    uint32_t tempo_us;   /* µs per quarter note (when is_tempo) */
    uint32_t off;        /* wire bytes offset into the pool (when !is_tempo) */
    uint16_t len;        /* wire byte count */
} rec_t;

/* ── A tiny growable byte/record buffer ────────────────────────────────── */
typedef struct { uint8_t* p; size_t len, cap; } bytebuf_t;
typedef struct { rec_t*   p; size_t len, cap; } recbuf_t;

static bool bb_push(bytebuf_t* b, const uint8_t* src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n) nc *= 2;
        uint8_t* np = realloc(b->p, nc);
        if (!np) return false;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return true;
}

static bool rb_push(recbuf_t* b, rec_t r)
{
    if (b->len + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        rec_t* np = realloc(b->p, nc * sizeof(rec_t));
        if (!np) return false;
        b->p = np; b->cap = nc;
    }
    b->p[b->len++] = r;
    return true;
}

/* ── Big-endian readers with bounds checks ─────────────────────────────── */
static bool rd_u32(const uint8_t* d, size_t size, size_t* pos, uint32_t* out)
{
    if (*pos + 4 > size) return false;
    *out = (uint32_t)d[*pos] << 24 | (uint32_t)d[*pos+1] << 16 |
           (uint32_t)d[*pos+2] << 8 | d[*pos+3];
    *pos += 4;
    return true;
}
static bool rd_u16(const uint8_t* d, size_t size, size_t* pos, uint16_t* out)
{
    if (*pos + 2 > size) return false;
    *out = (uint16_t)((uint16_t)d[*pos] << 8 | d[*pos+1]);
    *pos += 2;
    return true;
}

/* Variable-length quantity (max 4 bytes). */
static bool rd_vlq(const uint8_t* d, size_t size, size_t* pos, uint32_t* out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= size) return false;
        uint8_t b = d[(*pos)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;  /* malformed (5th continuation byte) */
}

/* Wire length of a channel message from its status high nibble (2 or 1 data). */
static int channel_msg_data(uint8_t status)
{
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        default: return -1;
    }
}

/* Sort by absolute tick, then by parse order (stable across tracks). */
static int rec_cmp(const void* a, const void* b)
{
    const rec_t* ra = a; const rec_t* rb = b;
    if (ra->tick < rb->tick) return -1;
    if (ra->tick > rb->tick) return 1;
    if (ra->seq  < rb->seq)  return -1;
    if (ra->seq  > rb->seq)  return 1;
    return 0;
}

/* Parse one MTrk body [pos, pos+len) into records; advance pos past it. */
static bool parse_track(const uint8_t* d, size_t size, size_t* pos, size_t tlen,
                        bytebuf_t* pool, recbuf_t* recs, uint32_t* seq)
{
    size_t end = *pos + tlen;
    if (end > size) return false;
    uint32_t tick = 0;
    uint8_t running = 0;

    while (*pos < end) {
        uint32_t delta;
        if (!rd_vlq(d, end, pos, &delta)) return false;
        tick += delta;
        if (*pos >= end) return false;

        uint8_t b = d[*pos];
        if (b == 0xFF) {
            /* Meta event: FF type vlqlen data */
            (*pos)++;
            if (*pos >= end) return false;
            uint8_t mtype = d[(*pos)++];
            uint32_t mlen;
            if (!rd_vlq(d, end, pos, &mlen)) return false;
            if (*pos + mlen > end) return false;
            if (mtype == 0x51 && mlen == 3) {
                uint32_t t = (uint32_t)d[*pos] << 16 |
                             (uint32_t)d[*pos+1] << 8 | d[*pos+2];
                rec_t r = { tick, (*seq)++, true, t, 0, 0 };
                if (!rb_push(recs, r)) return false;
            }
            *pos += mlen;
            running = 0;  /* meta cancels running status */
        } else if (b == 0xF0 || b == 0xF7) {
            /* SysEx (F0) or escape (F7): status + vlqlen + data */
            (*pos)++;
            uint32_t slen;
            if (!rd_vlq(d, end, pos, &slen)) return false;
            if (*pos + slen > end) return false;
            uint32_t off = (uint32_t)pool->len;
            if (!bb_push(pool, &b, 1)) return false;
            if (slen && !bb_push(pool, d + *pos, slen)) return false;
            rec_t r = { tick, (*seq)++, false, 0, off, (uint16_t)(slen + 1) };
            if (!rb_push(recs, r)) return false;
            *pos += slen;
            running = 0;  /* SysEx cancels running status */
        } else {
            /* Channel voice/mode message, possibly with running status. */
            uint8_t status;
            if (b & 0x80) { status = b; (*pos)++; running = status; }
            else          { status = running; if (!status) return false; }
            int ndata = channel_msg_data(status);
            if (ndata < 0) return false;
            if (*pos + (size_t)ndata > end) return false;
            uint8_t msg[3];
            msg[0] = status;
            for (int i = 0; i < ndata; i++) msg[1 + i] = d[(*pos)++];
            uint32_t off = (uint32_t)pool->len;
            if (!bb_push(pool, msg, (size_t)(ndata + 1))) return false;
            rec_t r = { tick, (*seq)++, false, 0, off, (uint16_t)(ndata + 1) };
            if (!rb_push(recs, r)) return false;
        }
    }
    *pos = end;
    return true;
}

bool smf_parse(const uint8_t* data, size_t size, smf_t* out)
{
    memset(out, 0, sizeof(*out));
    size_t pos = 0;

    /* Header: "MThd" len(=6) format ntracks division */
    if (size < 14 || memcmp(data, "MThd", 4) != 0) return false;
    pos = 4;
    uint32_t hlen; uint16_t format, ntracks; uint16_t division;
    if (!rd_u32(data, size, &pos, &hlen)) return false;
    if (!rd_u16(data, size, &pos, &format)) return false;
    if (!rd_u16(data, size, &pos, &ntracks)) return false;
    if (!rd_u16(data, size, &pos, &division)) return false;
    pos = 8 + hlen;  /* skip any extra header bytes */

    bytebuf_t pool = {0};
    recbuf_t  recs = {0};
    uint32_t  seq  = 0;
    bool ok = true;

    for (uint16_t t = 0; t < ntracks && pos + 8 <= size; t++) {
        if (memcmp(data + pos, "MTrk", 4) != 0) { ok = false; break; }
        pos += 4;
        uint32_t tlen;
        if (!rd_u32(data, size, &pos, &tlen)) { ok = false; break; }
        if (!parse_track(data, size, &pos, tlen, &pool, &recs, &seq)) {
            ok = false; break;
        }
    }
    if (!ok) { free(pool.p); free(recs.p); return false; }

    /* Merge all tracks onto one timeline (stable by tick, then parse order). */
    qsort(recs.p, recs.len, sizeof(rec_t), rec_cmp);

    /* SMPTE vs PPQN division → microseconds-per-tick model. */
    bool smpte = (division & 0x8000) != 0;
    double us_per_tick;
    uint32_t ppqn = 0;
    if (smpte) {
        int frames = -(int8_t)(division >> 8);     /* 24/25/29/30 */
        int subfr  = division & 0xFF;
        double fps = (frames == 29) ? 29.97 : (double)frames;
        us_per_tick = 1.0e6 / (fps * (double)subfr);
    } else {
        ppqn = division ? division : 480;
        us_per_tick = (double)SMF_DEFAULT_TEMPO_US / (double)ppqn;
    }

    /* Integrate the tempo map, emitting wire events with absolute µs. */
    smf_event_t* evs = NULL;
    size_t nev = 0, evcap = 0;
    uint32_t last_tick = 0;
    double   last_us = 0.0;

    for (size_t i = 0; i < recs.len; i++) {
        rec_t* r = &recs.p[i];
        double now_us = last_us + (double)(r->tick - last_tick) * us_per_tick;
        if (r->is_tempo) {
            last_us = now_us;
            last_tick = r->tick;
            if (!smpte && ppqn)
                us_per_tick = (double)r->tempo_us / (double)ppqn;
            continue;
        }
        if (nev + 1 > evcap) {
            evcap = evcap ? evcap * 2 : 128;
            smf_event_t* ne = realloc(evs, evcap * sizeof(smf_event_t));
            if (!ne) { free(evs); free(pool.p); free(recs.p); return false; }
            evs = ne;
        }
        evs[nev].t_us = (uint32_t)(now_us + 0.5);
        evs[nev].len  = r->len;
        evs[nev].off  = r->off;
        nev++;
    }
    free(recs.p);

    out->events   = evs;
    out->count    = nev;
    out->pool     = pool.p;
    out->pool_len = pool.len;
    out->format   = format;
    out->ntracks  = ntracks;
    out->total_us = nev ? evs[nev - 1].t_us + 1 : 0;
    return true;
}

bool smf_load(const char* path, smf_t* out)
{
    memset(out, 0, sizeof(*out));
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    uint8_t* buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    bool ok = (rd == (size_t)sz) && smf_parse(buf, (size_t)sz, out);
    free(buf);
    return ok;
}

void smf_free(smf_t* s)
{
    if (!s) return;
    free(s->events);
    free(s->pool);
    s->events = NULL;
    s->pool = NULL;
    s->count = s->pool_len = 0;
}
