/**
 * @file mea8000.c
 * @brief TMPI "Synthétiseur Vocal" — Philips/Signetics MEA 8000 formant core.
 * @author bmarty <bmarty@mailo.com>
 *
 * The formant-synthesis engine (quantization tables, second-order filter
 * cascade, frame decoder and sequencer) is a faithful C11 port of MAME's
 * src/devices/sound/mea8000.cpp:
 *
 *   license:BSD-3-Clause
 *   copyright-holders:Antoine Miné   (Copyright (C) Antoine Miné 2006)
 *   "Philips / Signetics MEA 8000 emulation. … The French company TMPI
 *    (Techni-musique & parole informatique) provided speech extensions for
 *    several 8-bit computers (Thomson, Amstrad, Oric)."
 *
 * The BSD-3-Clause licence permits reuse with attribution, retained here. The
 * integer (uint16) code path is ported (MEA8000_FLOAT_MODE undefined in MAME).
 *
 * Adaptation to Phosphoric: paced from CPU cycles (mea8000_tick, 64 kHz output),
 * resampled to the emulator rate (mea8000_generate) and mixed with the PSG. The
 * REQ pin is not modelled — the ORIC driver polls STATUS bit 7. There is NO
 * speech ROM: the host streams frame parameters.
 */

#include "io/mea8000.h"
#include "audio/audio.h"   /* AUDIO_SAMPLE_RATE */

#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------- */
/*  Quantization tables (MAME, from the MEA 8000 data sheet).                 */
/* ------------------------------------------------------------------------- */
static const int fm1_table[32] = {
    150,  162,  174,  188,  202,  217,  233,  250,
    267,  286,  305,  325,  346,  368,  391,  415,
    440,  466,  494,  523,  554,  587,  622,  659,
    698,  740,  784,  830,  880,  932,  988, 1047
};
static const int fm2_table[32] = {
    440,  466,  494,  523,  554,  587,  622,  659,
    698,  740,  784,  830,  880,  932,  988, 1047,
    1100, 1179, 1254, 1337, 1428, 1528, 1639, 1761,
    1897, 2047, 2214, 2400, 2609, 2842, 3105, 3400
};
static const int fm3_table[8] = {
    1179, 1337, 1528, 1761, 2047, 2400, 2842, 3400
};
static const int fm4_table[1] = { 3500 };

static const int bw_table[4] = { 726, 309, 125, 50 };

static const int ampl_table[16] = {
    0,   8,  11,  16,  22,  31,  44,   62,
    88, 125, 177, 250, 354, 500, 707, 1000
};

static const int pi_table[32] = {
    0, 1,  2,  3,  4,  5,  6,  7,
    8, 9, 10, 11, 12, 13, 14, 15,
    0 /* noise */, -15, -14, -13, -12, -11, -10, -9,
    -8, -7, -6, -5, -4, -3, -2, -1
};

/* ------------------------------------------------------------------------- */
/*  Table precomputation.                                                    */
/* ------------------------------------------------------------------------- */
static void mea8000_init_tables(mea8000_t* m)
{
    for (int i = 0; i < MEA8000_TABLE_LEN; i++) {
        double f = (double)i / MEA8000_F0;
        m->cos_table[i]  = (int)(2.0 * cos(2.0 * M_PI * f) * MEA8000_QUANT);
        m->exp_table[i]  = (int)(exp(-M_PI * f) * MEA8000_QUANT);
        m->exp2_table[i] = (int)(exp(-2.0 * M_PI * f) * MEA8000_QUANT);
    }
    /* Deterministic pseudo-random noise waveform in [-QUANT, QUANT). */
    uint32_t r = 0x12345678u;
    for (int i = 0; i < MEA8000_NOISE_LEN; i++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        m->noise_table[i] = (int)(r % (2 * MEA8000_QUANT)) - MEA8000_QUANT;
    }
    m->tables_ready = true;
}

/* ------------------------------------------------------------------------- */
/*  REQ / ready.                                                             */
/* ------------------------------------------------------------------------- */
static int mea8000_accept_byte(const mea8000_t* m)
{
    return m->state == MEA8000_STOPPED ||
           m->state == MEA8000_WAIT_FIRST ||
           (m->state == MEA8000_STARTED && m->bufpos < 4);
}

/* ------------------------------------------------------------------------- */
/*  DSP (integer path).                                                      */
/* ------------------------------------------------------------------------- */
static int mea8000_interp(const mea8000_t* m, uint16_t org, uint16_t dst)
{
    return org + (((int)dst - (int)org) * m->framepos) / (1 << m->framelog);
}

static int mea8000_filter_step(mea8000_t* m, int i, int input)
{
    int fm = mea8000_interp(m, m->f[i].last_fm, m->f[i].fm);
    int bw = mea8000_interp(m, m->f[i].last_bw, m->f[i].bw);
    if (fm < 0) fm = 0;
    if (fm >= MEA8000_TABLE_LEN) fm = MEA8000_TABLE_LEN - 1;
    if (bw < 0) bw = 0;
    if (bw >= MEA8000_TABLE_LEN) bw = MEA8000_TABLE_LEN - 1;
    int b = (m->cos_table[fm] * m->exp_table[bw]) / MEA8000_QUANT;
    int c = m->exp2_table[bw];
    int next_output = input + (b * m->f[i].output - c * m->f[i].last_output) / MEA8000_QUANT;
    m->f[i].last_output = m->f[i].output;
    m->f[i].output = next_output;
    return next_output;
}

static int mea8000_noise_gen(mea8000_t* m)
{
    m->phi = (m->phi + 1) % MEA8000_NOISE_LEN;
    return m->noise_table[m->phi];
}

static int mea8000_freq_gen(mea8000_t* m)
{
    int pitch = mea8000_interp(m, m->last_pitch, m->pitch);
    if (pitch < 0) pitch = 0;
    m->phi = (m->phi + (uint32_t)pitch) % MEA8000_F0;
    return (int)((m->phi % MEA8000_F0) * MEA8000_QUANT * 2 / MEA8000_F0) - MEA8000_QUANT;
}

static int mea8000_compute_sample(mea8000_t* m)
{
    int ampl = mea8000_interp(m, m->last_ampl, m->ampl);
    int out = m->noise ? mea8000_noise_gen(m) : mea8000_freq_gen(m);
    out = out * (ampl / 32);
    for (int i = 0; i < 4; i++)
        out = mea8000_filter_step(m, i, out);
    if (out >  32767) out =  32767;
    if (out < -32767) out = -32767;
    return out;
}

/* ------------------------------------------------------------------------- */
/*  Frame management.                                                        */
/* ------------------------------------------------------------------------- */
static void mea8000_shift_frame(mea8000_t* m)
{
    m->last_pitch = m->pitch;
    for (int i = 0; i < 4; i++) {
        m->f[i].last_bw = m->f[i].bw;
        m->f[i].last_fm = m->f[i].fm;
    }
    m->last_ampl = m->ampl;
}

static void mea8000_decode_frame(mea8000_t* m)
{
    int fd = (m->buf[3] >> 5) & 3;            /* 0=8ms,1=16,2=32,3=64 */
    int pi = pi_table[m->buf[3] & 0x1f] << fd;
    m->noise = ((m->buf[3] & 0x1f) == 16);
    m->pitch = (uint16_t)(m->last_pitch + pi);
    m->f[0].bw = (uint16_t)bw_table[m->buf[0] >> 6];
    m->f[1].bw = (uint16_t)bw_table[(m->buf[0] >> 4) & 3];
    m->f[2].bw = (uint16_t)bw_table[(m->buf[0] >> 2) & 3];
    m->f[3].bw = (uint16_t)bw_table[m->buf[0] & 3];
    m->f[3].fm = (uint16_t)fm4_table[0];
    m->f[2].fm = (uint16_t)fm3_table[m->buf[1] >> 5];
    m->f[1].fm = (uint16_t)fm2_table[m->buf[1] & 0x1f];
    m->f[0].fm = (uint16_t)fm1_table[m->buf[2] >> 3];
    m->ampl = (uint16_t)ampl_table[((m->buf[2] & 7) << 1) | (m->buf[3] >> 7)];
    m->framelog = (uint16_t)(fd + 6 + 3);     /* 64 samples/ms, ×8 supersample */
    m->framelength = (uint16_t)(1 << m->framelog);
    m->bufpos = 0;
}

static void mea8000_start_frame(mea8000_t* m)
{
    m->framepos = 0;
}

static void mea8000_stop_frame(mea8000_t* m)
{
    m->state = MEA8000_STOPPED;
    m->output = 0;
}

/* One 64 kHz output step (MAME timer_expire), pushed to the ring. */
static void mea8000_step(mea8000_t* m)
{
    int pos = m->framepos % MEA8000_SUPERSAMPLING;
    if (!pos) {
        m->lastsample = m->sample;
        m->sample = (int16_t)mea8000_compute_sample(m);
        m->output = m->lastsample;
    } else {
        m->output = m->lastsample +
                    (pos * (m->sample - m->lastsample)) / MEA8000_SUPERSAMPLING;
    }

    m->ring[m->rhead++ & MEA8000_RING_MASK] = (int16_t)m->output;

    m->framepos++;
    if (m->framepos >= m->framelength) {
        mea8000_shift_frame(m);
        if (m->bufpos == 4) {               /* a successor frame is ready */
            mea8000_decode_frame(m);
            mea8000_start_frame(m);
        } else if (m->cont) {               /* repeat mode */
            mea8000_start_frame(m);
        } else if (m->state == MEA8000_STARTED) { /* slow stop: fade out */
            m->ampl = 0;
            mea8000_start_frame(m);
            m->state = MEA8000_SLOWING;
        } else if (m->state == MEA8000_SLOWING) {
            mea8000_stop_frame(m);
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Public API                                                               */
/* ------------------------------------------------------------------------- */
void mea8000_reset(mea8000_t* m)
{
    m->state = MEA8000_STOPPED;
    memset(m->buf, 0, sizeof(m->buf));
    m->bufpos = 0;
    m->cont = m->roe = 0;
    m->framelength = m->framepos = m->framelog = 0;
    m->lastsample = m->sample = 0;
    m->output = 0;
    m->phi = 0;
    memset(m->f, 0, sizeof(m->f));
    m->last_ampl = m->ampl = 0;
    m->last_pitch = m->pitch = 0;
    m->noise = 0;
    m->rhead = m->rtail = 0;
    m->cycle_acc = 0;
    m->resample_acc = 0;
    m->last_out = 0;
}

void mea8000_init(mea8000_t* m, uint16_t base_addr)
{
    memset(m, 0, sizeof(*m));
    m->base_addr = base_addr ? base_addr : MEA8000_BASE_DEFAULT;
    mea8000_init_tables(m);
    mea8000_reset(m);
}

uint8_t mea8000_read(mea8000_t* m, uint16_t addr)
{
    if (addr != m->base_addr && addr != (uint16_t)(m->base_addr + 1))
        return 0xFF;
    /* STATUS: bit 7 = ready to accept the next byte/frame. */
    return (uint8_t)(mea8000_accept_byte(m) << 7);
}

void mea8000_write(mea8000_t* m, uint16_t addr, uint8_t data)
{
    int offset = (int)addr - (int)m->base_addr;
    if (offset == 0) {                       /* DATA register */
        if (m->state == MEA8000_STOPPED) {
            m->pitch = (uint16_t)(2 * data); /* initial pitch byte */
            m->state = MEA8000_WAIT_FIRST;
            m->bufpos = 0;
        } else if (m->bufpos == 4) {
            /* frame buffer full: overflow, drop (host should have polled) */
        } else {
            m->buf[m->bufpos++] = data;
            if (m->bufpos == 4 && m->state == MEA8000_WAIT_FIRST) {
                uint16_t old_pitch = m->pitch;
                m->last_pitch = old_pitch;
                mea8000_decode_frame(m);
                mea8000_shift_frame(m);
                m->last_pitch = old_pitch;
                m->ampl = 0;                 /* fade-in the first frame */
                mea8000_start_frame(m);
                m->state = MEA8000_STARTED;
            }
        }
    } else if (offset == 1) {                /* COMMAND register */
        int stop = (data >> 4) & 1;
        if (data & 8) m->cont = (data >> 2) & 1;
        if (data & 2) m->roe  = data & 1;
        if (stop) mea8000_stop_frame(m);
    }
}

void mea8000_tick(mea8000_t* m, int cycles)
{
    if (cycles <= 0) return;
    /* 64000 output samples per 1e6 CPU cycles → 64/1000 sample per cycle. */
    m->cycle_acc += cycles * 64;
    int nsamp = m->cycle_acc / 1000;
    if (nsamp <= 0) return;
    m->cycle_acc -= nsamp * 1000;

    for (int i = 0; i < nsamp; i++) {
        if (m->state == MEA8000_STARTED || m->state == MEA8000_SLOWING)
            mea8000_step(m);
        else
            m->ring[m->rhead++ & MEA8000_RING_MASK] = 0;  /* idle → silence */
    }
    uint32_t occ = m->rhead - m->rtail;
    if (occ > MEA8000_RING_SIZE)
        m->rtail = m->rhead - MEA8000_RING_SIZE;
}

void mea8000_generate(mea8000_t* m, int16_t* out, int count)
{
    for (int i = 0; i < count; i++) {
        out[i] = m->last_out;
        m->resample_acc += MEA8000_OUT_RATE;            /* 64000 */
        while (m->resample_acc >= AUDIO_SAMPLE_RATE) {  /* 44100 */
            m->resample_acc -= AUDIO_SAMPLE_RATE;
            if (m->rtail != m->rhead)
                m->last_out = m->ring[m->rtail++ & MEA8000_RING_MASK];
            else
                m->last_out = 0;
        }
    }
}

bool mea8000_speaking(const mea8000_t* m)
{
    return m->state == MEA8000_STARTED || m->state == MEA8000_SLOWING;
}
