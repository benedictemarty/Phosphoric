/**
 * @file cassette.h
 * @brief Signal-level cassette interface (VIA CB1 + Timer 2)
 * @author bmarty <bmarty@mailo.com>
 * @version 1.0.0-alpha
 *
 * Sprint 90 — Signal-level tape emulation. Instead of patching the ROM CLOAD
 * routines (getsync/readbyte), this generates the actual tape waveform on the
 * VIA CB1 input so the *real* ROM read routine (and any custom loader that
 * reads CB1 directly, e.g. protected games) sees a genuine signal — exactly
 * like a real machine or Euphoric.
 *
 * ORIC read protocol (reverse-engineered from BASIC 1.0 ROM $E67D):
 *   - Tape signal drives VIA CB1; each active edge sets IFR bit 4.
 *   - The ROM clears the flag by reading ORB ($0300), waits for the next edge,
 *     and times the interval with Timer 2 (reload $FF, read T2C-H).
 *   - Interval < ~512 cycles -> "short" (bit '1', 2400 Hz),
 *     interval >= 512 cycles -> "long"  (bit '0', 1200 Hz).
 *   - Byte frame = 14 bits, LSB first: start(0) . 8 data . odd parity . stop(1111).
 */

#ifndef CASSETTE_H
#define CASSETTE_H

#include <stdint.h>
#include <stdbool.h>

/* Tape tone half-periods, in CPU (phi2 @ 1 MHz) cycles. A full same-polarity
 * CB1 period spans two half-pulses; the ROM measures one period per bit.
 *   bit '1' : period 2*CAS_HALF_ONE  (short, < ROM 512-cycle threshold)
 *   bit '0' : CAS_HALF_ONE + CAS_HALF_LONG period (long, >= threshold)
 * Values mirror the ROM CSAVE writer ($E619: 208 / 416 half-pulses -> the read
 * side sees full-bit periods of ~416 and ~624 cycles). Tunable. */
/* Canonical ORIC encoding (mirrors ROM CSAVE $E5F9/$E619): the FIRST half of
 * every bit is a constant short pulse; only the SECOND half encodes the value.
 * This anchors the ROM's timing edge at a constant phase, so consecutive edges
 * are exactly one bit-period apart regardless of neighbouring bits.
 *   bit '1' : 208 + 208 = 416-cycle period (< ROM ~512 threshold => short) */
#define CAS_HALF_ONE   208   /* first half (always) and second half of a '1' bit */
#define CAS_HALF_LONG  416   /* second half of a '0' bit -> 624-cycle period (long)   */

/* Pilot leader: a run of 0x16 sync frames emitted before the tape byte stream.
 * The ROM getsync routine bit-locks on a 0x16 then confirms three more via
 * framed readbyte, so several are needed (real CSAVE writes ~259). */
#define CAS_LEADER_SYNCS   64
#define CAS_SYNC_BYTE      0x16

/* 14-bit frame layout constants. */
#define CAS_FRAME_BITS   14

/**
 * @brief Signal-level cassette generator state.
 *
 * The byte stream is the raw TAP buffer (emu->tapebuf): sync bytes 0x16,
 * marker 0x24, header and data — the real ROM getsync/readbyte consume it.
 */
typedef struct cassette_s {
    bool           signal_mode;   /**< Signal-level path active (vs ROM patch) */
    bool           motor_on;      /**< Cassette motor (VIA ORB PB6) */

    const uint8_t* buf;           /**< Tape byte stream (= emu->tapebuf) */
    int            len;           /**< Stream length (= emu->tapelen) */

    /* Waveform position */
    int            byte_pos;      /**< Next byte index into buf */
    int            bit_pos;       /**< Current bit 0..13 in the 14-bit frame */
    uint16_t       frame;         /**< Current 14-bit frame being shifted out */
    int            half;          /**< Half-pulse within current bit (0/1) */
    int            leader_left;   /**< Remaining leader half-pulses */
    int32_t        cyc_to_edge;   /**< Cycles until next CB1 transition */
    bool           cb1_level;     /**< Current CB1 line level */
    bool           finished;      /**< Whole tape emitted */
    bool           started;       /**< Playback armed (rewound on first read) */
} cassette_t;

/* Forward decl to avoid pulling emulator.h into this header. */
struct via6522_s;

/** Initialise / reset the generator (idle, motor off). */
void cassette_init(cassette_t* c);
void cassette_reset(cassette_t* c);

/** Enable signal-level mode over a tape byte stream (buf/len borrowed). */
void cassette_signal_begin(cassette_t* c, const uint8_t* buf, int len);

/** Set cassette motor line (from VIA ORB PB6). Signal only advances when on. */
void cassette_set_motor(cassette_t* c, bool on);

/** Rewind the waveform to the start of the byte stream. */
void cassette_rewind(cassette_t* c);

/**
 * @brief Advance the tape signal by @p cycles CPU cycles.
 *
 * Toggles VIA CB1 at the scheduled pulse edges so the ROM read routine (and
 * custom loaders) sample a genuine waveform. No-op unless signal mode is on
 * and the motor is running.
 */
void cassette_tick(cassette_t* c, struct via6522_s* via, int cycles);

/** Encode one byte to its 14-bit tape frame (start/data/parity/stop). */
uint16_t cassette_encode_frame(uint8_t byte);

#endif /* CASSETTE_H */
