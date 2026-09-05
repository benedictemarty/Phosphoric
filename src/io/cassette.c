/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cassette.c
 * @brief Signal-level cassette generator (VIA CB1 + Timer 2)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-07-04
 * @version 1.0.0-alpha
 *
 * Sprint 90 — replaces the former stub with a real tape waveform generator.
 * See cassette.h for the reverse-engineered ORIC read protocol.
 */

#include <stddef.h>
#include <stdlib.h>

#include "io/cassette.h"
#include "io/via6522.h"

/* Idle level of the tape line. The ROM samples same-polarity edges, so the
 * resting level only matters for the very first transition. */
#define CAS_IDLE_HIGH  true

void cassette_init(cassette_t* c) {
    if (!c) return;
    c->signal_mode = false;
    c->motor_on    = false;
    c->buf = NULL;
    c->len = 0;
    c->byte_pos = 0;
    c->bit_pos  = CAS_FRAME_BITS;   /* force loading a new frame first */
    c->frame    = 0;
    c->half     = 0;
    c->leader_left = 0;
    c->cyc_to_edge = 0;
    c->cb1_level   = CAS_IDLE_HIGH;
    c->finished    = false;
    c->started     = false;
}

void cassette_reset(cassette_t* c) {
    cassette_init(c);
}

uint16_t cassette_encode_frame(uint8_t byte) {
    /* Odd parity over the 8 data bits (matches the ORIC ROM tape framing):
     * parity bit chosen so that (data ones + parity) is odd. */
    uint8_t ones = 0;
    for (int i = 0; i < 8; i++) ones += (uint8_t)((byte >> i) & 1u);
    uint16_t parity = (uint16_t)(ones & 1u);

    /* bit0 = start (0), bits1..8 = data LSB first, bit9 = parity,
     * bits10..13 = stop (1). */
    uint16_t frame = (uint16_t)((byte << 1) & 0x01FEu);
    frame |= (uint16_t)(parity << 9);
    frame |= 0x3C00u;   /* four stop bits */
    return frame;       /* bit0 (start) is already 0 */
}

void cassette_rewind(cassette_t* c) {
    if (!c) return;
    c->byte_pos = 0;
    c->bit_pos  = CAS_FRAME_BITS;
    c->half     = 0;
    c->leader_left = CAS_LEADER_SYNCS;
    c->cyc_to_edge = 0;
    c->cb1_level   = CAS_IDLE_HIGH;
    c->finished    = false;
}

void cassette_signal_begin(cassette_t* c, const uint8_t* buf, int len) {
    if (!c) return;
    c->signal_mode = true;
    c->buf = buf;
    c->len = len;
    cassette_rewind(c);
}

void cassette_set_motor(cassette_t* c, bool on) {
    if (!c) return;
    c->motor_on = on;
}

/* Emit one half-pulse: drive an EXPLICIT CB1 level (not a free toggle) so the
 * waveform stays phase-anchored across bits of differing period. Each bit is
 * two half-pulses: half 0 drives the line LOW, half 1 drives it HIGH — so the
 * rising edge (which the ROM times) lands at mid-bit and consecutive rising
 * edges are exactly one bit-period apart, regardless of neighbouring bits.
 * Returns the number of CPU cycles this half-pulse lasts, and *level. */
static int32_t cassette_step(cassette_t* c, bool* level) {
    /* Load the next 14-bit frame when the current one is done. Frames come from
     * the pilot leader (a run of 0x16 sync bytes) first, then the tape buffer. */
    if (c->bit_pos >= CAS_FRAME_BITS) {
        uint8_t b;
        if (c->leader_left > 0) {
            b = CAS_SYNC_BYTE;
            c->leader_left--;
        } else if (c->byte_pos < c->len) {
            b = c->buf[c->byte_pos++];
        } else {
            c->finished = true;
            *level = true;
            return 100000;   /* park far away; motor gate stops us anyway */
        }
        c->frame   = cassette_encode_frame(b);
        c->bit_pos = 0;
        c->half    = 0;
    }
    int bit = (c->frame >> c->bit_pos) & 1;

    /* First half: always LOW for CAS_HALF_ONE. Second half: HIGH, length
     * encodes the bit (short for '1', long for '0'). The rising edge therefore
     * always lands CAS_HALF_ONE into the bit — a constant phase. */
    int32_t dur;
    if (c->half == 0) {
        *level = false;
        dur = CAS_HALF_ONE;
    } else {
        *level = true;
        dur = bit ? CAS_HALF_ONE : CAS_HALF_LONG;
    }

    if (++c->half >= 2) {   /* both half-pulses of this bit emitted */
        c->half = 0;
        c->bit_pos++;
    }
    return dur;
}

/* ---- Tape-OUT capture/decoder (voie A CSAVE) --------------------------- */

void tape_capture_init(tape_capture_t* tc) {
    if (!tc) return;
    tc->active = false;
    tc->primed = false;
    tc->last_pb7 = false;
    tc->have_prev_edge = false;
    tc->prev_edge_cyc = 0;
    tc->state = 0;
    tc->bitcount = 0;
    tc->cur_byte = 0;
    tc->out = NULL;
    tc->out_len = 0;
    tc->out_cap = 0;
}

void tape_capture_begin(tape_capture_t* tc) {
    if (!tc) return;
    tape_capture_init(tc);
    tc->out_cap = 1024;
    tc->out = (uint8_t*)malloc((size_t)tc->out_cap);
    tc->out_len = 0;
    tc->active = (tc->out != NULL);
    /* last_pb7 is seeded from the first real sample (tc->primed), avoiding a
     * spurious startup edge regardless of the idle PB7 level. */
}

void tape_capture_free(tape_capture_t* tc) {
    if (!tc) return;
    if (tc->out) free(tc->out);
    tc->out = NULL;
    tc->out_cap = tc->out_len = 0;
    tc->active = false;
}

static void tape_capture_emit(tape_capture_t* tc, uint8_t b) {
    if (tc->out_len >= tc->out_cap) {
        int ncap = tc->out_cap ? tc->out_cap * 2 : 1024;
        uint8_t* n = (uint8_t*)realloc(tc->out, (size_t)ncap);
        if (!n) return;              /* drop byte rather than crash on OOM */
        tc->out = n;
        tc->out_cap = ncap;
    }
    tc->out[tc->out_len++] = b;
}

/* Feed one decoded bit into the frame re-assembler. Mirrors ROM GetTapeByte
 * ($E6C9 on BASIC 1.1): it does NOT clock a fixed-length frame — it hunts the
 * start bit. After a byte it burns one period (parity) then skips the short '1'
 * periods (stop bits, however many) and takes the first long '0' period as the
 * next start. Counting a fixed number of stop bits would drift the framing by
 * one bit per frame whenever the real stop count differs from the model (3.5
 * real vs 4 encoded, or a stop truncated at end of CSAVE) — which is what
 * produced the doubled/alternating byte patterns. */
static void tape_capture_bit(tape_capture_t* tc, int bit) {
    switch (tc->state) {
    case 0: /* hunt start: skip short '1's, the first long '0' is the start */
        if (bit == 0) {
            tc->state = 1;
            tc->bitcount = 0;
            tc->cur_byte = 0;
        }
        break;
    case 1: /* 8 data bits, LSB first (ROR-style) */
        tc->cur_byte = (uint8_t)((tc->cur_byte >> 1) | (bit ? 0x80 : 0x00));
        if (++tc->bitcount >= 8) {
            tape_capture_emit(tc, tc->cur_byte);   /* byte complete */
            tc->state = 2;                          /* burn parity next */
        }
        break;
    case 2: /* burn exactly one period (the parity bit, any value), then hunt
             * the next start — stop-bit count is irrelevant to the framing */
        tc->state = 0;
        break;
    }
}

void tape_capture_sample(tape_capture_t* tc, via6522_t* via, uint64_t cyc) {
    if (!tc || !tc->active || !via) return;
    bool pb7 = via_get_pb7(via);
    /* Seed the level from the first real sample so an idle-level mismatch does
     * not fabricate a spurious startup edge (which would inject one garbage
     * bit and desync the framing). */
    if (!tc->primed) {
        tc->primed = true;
        tc->last_pb7 = pb7;
        return;
    }
    /* CSAVE emits TWO half-pulses per bit (low 208, then high 208 for '1' or
     * 416 for '0') — the same waveform cassette_step() generates. The ROM times
     * RISING edge to RISING edge, so only rising edges are counted; the period
     * between consecutive rising edges is the full bit period (416 '1' / 624
     * '0'). Counting every edge would instead measure half-pulses (208/416,
     * both below the 512 threshold → every bit read as '1'), which breaks the
     * decode. Confirmed against the FPGA RTL injector and the ROM disassembly
     * of GetTapeByte ($E6C9, Timer 2, threshold ~520). */
    bool edge = (!tc->last_pb7 && pb7);
    tc->last_pb7 = pb7;
    if (!edge) return;
    if (!tc->have_prev_edge) {
        tc->have_prev_edge = true;
        tc->prev_edge_cyc = cyc;
        return;
    }
    uint64_t period = cyc - tc->prev_edge_cyc;
    tc->prev_edge_cyc = cyc;
    int bit = (period < CAS_PERIOD_THRESH) ? 1 : 0;
    tape_capture_bit(tc, bit);
}

/* ------------------------------------------------------------------------ */

void cassette_tick(cassette_t* c, via6522_t* via, int cycles) {
    if (!c || !via) return;
    if (!c->signal_mode || !c->motor_on || c->finished || c->len <= 0) return;

    c->cyc_to_edge -= cycles;
    /* A single instruction spans a handful of cycles; guard against emitting a
     * runaway number of edges if a long gap ever accumulates. */
    int guard = 0;
    while (c->cyc_to_edge <= 0 && guard++ < 64) {
        bool level = c->cb1_level;
        int32_t dur = cassette_step(c, &level);
        c->cb1_level = level;
        via_set_cb1(via, level);
        c->cyc_to_edge += dur;
        if (c->finished) break;
    }
}
