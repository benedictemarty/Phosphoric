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
