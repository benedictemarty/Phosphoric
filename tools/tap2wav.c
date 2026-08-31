/**
 * @file tap2wav.c
 * @brief Convert an ORIC .TAP file to a cassette-audio .WAV (fast/standard mode).
 * @author bmarty <bmarty@mailo.com>
 *
 * Produces the real ORIC cassette waveform so the .wav can be played into a
 * genuine machine's tape input (or archived as audio). The bit/byte encoding is
 * exactly what Phosphoric's signal-level cassette generator emits (src/io/
 * cassette.c, mirroring the ROM CSAVE writer at $E619):
 *
 *   - 14-bit frame, LSB first: start(0) . 8 data . odd parity . 4 stop(1).
 *   - Each bit = two half-pulses: first half LOW for 208 cycles (constant),
 *     second half HIGH for 208 cycles (bit '1') or 416 cycles (bit '0').
 *     Read side sees full-bit periods ~416 ('1') / ~624 ('0'), split at the
 *     ROM's ~512-cycle threshold.
 *   - A pilot leader of 0x16 sync frames precedes the byte stream.
 *
 * CPU/phi2 clock is 1 MHz, so a cycle count converts directly to time.
 *
 * Usage: tap2wav IN.tap OUT.wav [--rate HZ] [--leader N] [--amp N]
 *                               [--lead-silence MS] [--tail-silence MS]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ORIC_CPU_HZ    1000000
#define CAS_HALF_ONE   208     /* first half (always) + second half of a '1' bit  */
#define CAS_HALF_LONG  416     /* second half of a '0' bit                        */
#define CAS_FRAME_BITS 14
#define CAS_SYNC_BYTE  0x16

/* Encode one byte to its 14-bit ORIC tape frame — verbatim from Phosphoric's
 * cassette_encode_frame() (src/io/cassette.c): start(0), 8 data LSB-first,
 * odd parity (bit 9), four stop bits (1). */
static uint16_t encode_frame(uint8_t byte)
{
    uint8_t ones = 0;
    for (int i = 0; i < 8; i++) ones += (uint8_t)((byte >> i) & 1u);
    uint16_t parity = (uint16_t)(ones & 1u);
    uint16_t frame = (uint16_t)((byte << 1) & 0x01FEu);
    frame |= (uint16_t)(parity << 9);
    frame |= 0x3C00u;   /* four stop bits */
    return frame;
}

/* ── WAV output (mono, 16-bit PCM) ──────────────────────────────────────── */
typedef struct {
    FILE*    fp;
    uint32_t rate;
    uint32_t nsamples;
} wav_t;

static void wr16(FILE* f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void wr32(FILE* f, uint32_t v) {
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
}

static int wav_open(wav_t* w, const char* path, uint32_t rate)
{
    w->fp = fopen(path, "wb");
    if (!w->fp) return 0;
    w->rate = rate;
    w->nsamples = 0;
    /* Placeholder header (sizes patched on close). */
    fwrite("RIFF", 1, 4, w->fp); wr32(w->fp, 0);
    fwrite("WAVE", 1, 4, w->fp);
    fwrite("fmt ", 1, 4, w->fp); wr32(w->fp, 16);
    wr16(w->fp, 1);              /* PCM            */
    wr16(w->fp, 1);              /* mono           */
    wr32(w->fp, rate);
    wr32(w->fp, rate * 2);       /* byte rate      */
    wr16(w->fp, 2);              /* block align    */
    wr16(w->fp, 16);             /* bits/sample    */
    fwrite("data", 1, 4, w->fp); wr32(w->fp, 0);
    return 1;
}

static void wav_write(wav_t* w, int16_t s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) wr16(w->fp, (uint16_t)s);
    w->nsamples += n;
}

static void wav_close(wav_t* w)
{
    uint32_t data_bytes = w->nsamples * 2;
    fseek(w->fp, 4, SEEK_SET);           wr32(w->fp, 36 + data_bytes);
    fseek(w->fp, 40, SEEK_SET);          wr32(w->fp, data_bytes);
    fclose(w->fp);
}

/* ── Waveform generation ────────────────────────────────────────────────── */
static int16_t g_amp = 16000;

/* Emit `cyc` CPU cycles at the given level, converting to samples with a
 * fractional-cycle carry so timing stays exact over the whole stream. */
static void emit(wav_t* w, int level, uint32_t cyc, int64_t* carry)
{
    int64_t total = (int64_t)cyc * w->rate + *carry;
    int64_t n = total / ORIC_CPU_HZ;
    *carry = total % ORIC_CPU_HZ;
    wav_write(w, level ? g_amp : (int16_t)(-g_amp), (uint32_t)n);
}

static void emit_byte(wav_t* w, uint8_t b, int64_t* carry)
{
    uint16_t frame = encode_frame(b);
    for (int bp = 0; bp < CAS_FRAME_BITS; bp++) {
        int bit = (frame >> bp) & 1;
        emit(w, 0, CAS_HALF_ONE, carry);                       /* half 0: LOW  */
        emit(w, 1, bit ? CAS_HALF_ONE : CAS_HALF_LONG, carry); /* half 1: HIGH */
    }
}

int main(int argc, char** argv)
{
    const char* in = NULL;
    const char* out = NULL;
    uint32_t rate = 44100;
    uint32_t leader = 512;         /* pilot 0x16 sync frames */
    uint32_t lead_ms = 200, tail_ms = 500;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--rate")         && i + 1 < argc) rate    = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--leader")       && i + 1 < argc) leader  = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--amp")          && i + 1 < argc) g_amp   = (int16_t)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--lead-silence") && i + 1 < argc) lead_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--tail-silence") && i + 1 < argc) tail_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (argv[i][0] == '-') { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 2; }
        else if (!in)  in  = argv[i];
        else if (!out) out = argv[i];
    }
    if (!in || !out) {
        fprintf(stderr,
            "Usage: %s IN.tap OUT.wav [--rate HZ] [--leader N] [--amp N]\n"
            "                        [--lead-silence MS] [--tail-silence MS]\n"
            "Convert an ORIC .TAP to cassette-audio .WAV (fast/standard encoding).\n",
            argv[0]);
        return 2;
    }

    FILE* f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "tap2wav: cannot open %s\n", in); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "tap2wav: empty/invalid file %s\n", in); fclose(f); return 1; }
    uint8_t* tap = (uint8_t*)malloc((size_t)sz);
    if (!tap || fread(tap, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "tap2wav: read error\n"); fclose(f); free(tap); return 1;
    }
    fclose(f);

    wav_t w;
    if (!wav_open(&w, out, rate)) { fprintf(stderr, "tap2wav: cannot write %s\n", out); free(tap); return 1; }

    /* Leading silence. */
    wav_write(&w, 0, (uint32_t)((uint64_t)lead_ms * rate / 1000));

    int64_t carry = 0;
    /* Pilot leader of framed 0x16 sync bytes. */
    for (uint32_t i = 0; i < leader; i++) emit_byte(&w, CAS_SYNC_BYTE, &carry);
    /* The TAP byte stream (already contains its own 0x16.. 0x24 header + data). */
    for (long i = 0; i < sz; i++) emit_byte(&w, tap[i], &carry);

    /* Trailing silence. */
    wav_write(&w, 0, (uint32_t)((uint64_t)tail_ms * rate / 1000));

    wav_close(&w);
    double secs = (double)w.nsamples / rate;
    printf("tap2wav: %s (%ld bytes) -> %s  [%.2f s @ %u Hz, leader %u, amp %d]\n",
           in, sz, out, secs, rate, leader, g_amp);
    free(tap);
    return 0;
}
