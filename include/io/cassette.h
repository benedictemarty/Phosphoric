/* SPDX-License-Identifier: EUPL-1.2 */
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
    bool           free_gate;     /**< --tape-signal-free : gate le moteur sur ORB PB6
                                       (moteur ROM) au lieu du PC 1.1 -> supporte les
                                       ROM clean-room (layout different). */
} cassette_t;

/* Forward decl to avoid pulling emulator.h into this header. */
struct via6522_s;

/* Bit-period threshold (CPU cycles) separating a short '1' (~416) from a long
 * '0' (~624). Mirrors the ROM read decoder (`CLD_THRESH` scaled) and the write
 * half-periods above: 416 < 512 <= 624. */
#define CAS_PERIOD_THRESH  512

/**
 * @brief Tape-OUT capture/decoder (voie A CSAVE verification).
 *
 * Samples the VIA PB7 line (driven by Timer 1 in ACR bit7 mode — the Oric
 * cassette WRITE output) and reconstructs the .TAP byte stream the ROM emits,
 * by timing PB7 rising edges (period < CAS_PERIOD_THRESH => bit '1', else '0')
 * and re-framing 14-bit frames (start / 8 data LSB / parity / 4 stop), the
 * inverse of the writer. The decoded bytes are the same layout a --tape-signal
 * load would consume, so a CSAVE->capture->CLOAD round-trip is verifiable.
 */
typedef struct tape_capture_s {
    bool      active;         /**< Capture enabled (--tape-out-capture) */
    bool      primed;         /**< last_pb7 seeded from a real sample (no spurious
                                   startup edge) */
    bool      last_pb7;       /**< Previous PB7 sample (edge detection) */
    bool      have_prev_edge; /**< A prior rising edge exists (period valid) */
    uint64_t  prev_edge_cyc;  /**< Cycle of the previous PB7 rising edge */

    /* Frame re-assembly (mirror of ROM read_byte). */
    int       state;          /**< 0=seek start bit, 1=data bits, 2=skip parity/stop */
    int       bitcount;       /**< Bits accumulated in current phase */
    uint8_t   cur_byte;       /**< Byte being shifted in (LSB first) */

    /* Decoded output (grown as bytes are recovered). */
    uint8_t*  out;            /**< Reconstructed .TAP bytes */
    int       out_len;        /**< Bytes decoded so far */
    int       out_cap;        /**< Allocated capacity */
} tape_capture_t;

/** Initialise a capture (inactive, empty). */
void tape_capture_init(tape_capture_t* tc);
/** Arm capture: allocate the output buffer and reset the decoder. */
void tape_capture_begin(tape_capture_t* tc);
/** Free the output buffer. */
void tape_capture_free(tape_capture_t* tc);
/**
 * @brief Sample PB7 at absolute cycle @p cyc and decode any completed byte.
 * No-op unless armed. Call once per CPU cycle-tick.
 */
void tape_capture_sample(tape_capture_t* tc, struct via6522_s* via, uint64_t cyc);

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
