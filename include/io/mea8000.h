/**
 * @file mea8000.h
 * @brief TMPI "Synthétiseur Vocal" — Philips/Signetics MEA 8000 formant speech.
 * @author bmarty <bmarty@mailo.com>
 *
 * TMPI (Techni-Musique et Parole Informatique) speech extension for the ORIC,
 * built around the Philips/Signetics MEA 8000 formant speech synthesizer.
 * Attested by period reviews — Théoric n°26 ("Techni-Musique et Parole
 * Informatique … synthétiseur vocal proposé par TMPI … organisé autour d'un
 * MEA 8000") and CEO-Mag ("a synthesizer marketed by … TMPI … based on the
 * MEA 8000 … Oric expansion bus"). Unlike the SP0256 (Mageco card, [[sp0256]]),
 * the MEA 8000 has NO speech ROM: the host streams frame parameters and the
 * chip synthesises them, so no ROM file is needed.
 *
 * Interface (2 registers, A0-addressed). The commercial TMPI card decodes at
 * $03FE/$03FF — CONFIRMED in-game: the TMPI demo SYNTHOR ("DÉMONSTRATION DE
 * CHANT PAR PHONÈMES") streams 4-byte frames to $03FE (data) and writes $03FF
 * (command) starting with $1A, the MEA 8000 power-on command from the data
 * sheet. (A MAGECO MEA8000-ORIC *prototype* schematic instead used $03F0/$03F1;
 * the base is configurable via --mea8000-addr.)
 *   write $03FE (A0=0) : DATA — first byte after a stop is the initial pitch;
 *                        subsequent bytes fill 4-byte frames.
 *   write $03FF (A0=1) : COMMAND — bit4 STOP, bit3→bit2 CONT (repeat), bit1→bit0
 *                        ROE (req enable).
 *   read  $03FE/$03FF  : STATUS — bit 7 = ready to accept the next byte/frame.
 * The driver polls this: POKE the byte, then PEEK until D7=1.
 *
 * Synthesis is a faithful C11 port of MAME's src/devices/sound/mea8000.cpp
 * (license BSD-3-Clause, copyright Antoine Miné 2006): a sawtooth (voiced) or
 * noise (unvoiced) source through a cascade of 4 second-order formant filters
 * with programmable frequency/bandwidth, all parameters linearly interpolated
 * over the 8/16/32/64 ms frame. Runs internally at F0 = 8 kHz (3.84 MHz / 480),
 * supersampled ×8 to 64 kHz, then resampled to the emulator rate and mixed with
 * the AY-3-8910 PSG. The REQ output pin is not modelled (the CPU polls STATUS).
 */

#ifndef MEA8000_H
#define MEA8000_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration for emulator (avoids circular include) */
typedef struct emulator_s emulator_t;

#define MEA8000_BASE_DEFAULT   0x03FE   /* TMPI card: data $03FE (A0=0), cmd $03FF (A0=1)
                                         * — confirmed in-game (SYNTHOR). $03F0 = MAGECO proto. */

#define MEA8000_F0             8000     /* internal digital-filter rate (3.84 MHz/480) */
#define MEA8000_SUPERSAMPLING  8        /* output at 8 × F0 = 64 kHz               */
#define MEA8000_OUT_RATE       (MEA8000_F0 * MEA8000_SUPERSAMPLING) /* 64000 Hz    */
#define MEA8000_QUANT          512      /* fixed-point amplitude scale             */
#define MEA8000_TABLE_LEN      3600     /* freq→coefficient tables (max fm 3500)   */
#define MEA8000_NOISE_LEN      8192     /* noise waveform table                    */

/* Output ring for generated 64 kHz samples (power of two). */
#define MEA8000_RING_SIZE      8192
#define MEA8000_RING_MASK      (MEA8000_RING_SIZE - 1)

/* Frame-sequencer state (MAME mea8000_state). */
typedef enum {
    MEA8000_STOPPED = 0,   /* idle, sequencer off                                 */
    MEA8000_WAIT_FIRST,    /* got pitch, waiting for the first full frame         */
    MEA8000_STARTED,       /* playing a frame                                     */
    MEA8000_SLOWING        /* repeating last frame with fading amplitude          */
} mea8000_state_t;

/* One of the 4 cascaded second-order formant filters. */
typedef struct {
    uint16_t fm, last_fm;      /* centre frequency, Hz (also table index)         */
    uint16_t bw, last_bw;      /* bandwidth, Hz (also table index)                */
    int32_t  output, last_output; /* filter state                                 */
} mea8000_filter_t;

typedef struct mea8000_s {
    uint16_t base_addr;              /* data reg; command = base_addr + 1          */

    mea8000_state_t state;
    uint8_t  buf[4];                 /* 4 bytes forming a frame                    */
    uint8_t  bufpos;                 /* next byte slot                             */
    uint8_t  cont;                   /* 0=stop / 1=repeat last frame when starved  */
    uint8_t  roe;                    /* req-output enable (unmodelled)             */

    uint16_t framelength;            /* in 64 kHz samples                          */
    uint16_t framepos;
    uint16_t framelog;               /* log2(framelength)                          */

    int16_t  lastsample, sample;     /* F0 samples, interpolated ×8                */
    int32_t  output;                 /* current 64 kHz output sample               */
    uint32_t phi;                    /* sawtooth/noise phase                       */

    mea8000_filter_t f[4];

    uint16_t last_ampl, ampl;        /* amplitude ×1000                            */
    uint16_t last_pitch, pitch;      /* sawtooth pitch, Hz                         */
    uint8_t  noise;                  /* current frame is unvoiced                  */

    /* Precomputed coefficient / noise tables (built once in init). */
    int cos_table[MEA8000_TABLE_LEN];
    int exp_table[MEA8000_TABLE_LEN];
    int exp2_table[MEA8000_TABLE_LEN];
    int noise_table[MEA8000_NOISE_LEN];
    bool tables_ready;

    /* 64 kHz output ring + CPU-cycle pacing + resampling to the emulator rate. */
    int16_t  ring[MEA8000_RING_SIZE];
    uint32_t rhead, rtail;
    int32_t  cycle_acc;              /* fractional CPU-cycle accumulator           */
    uint32_t resample_acc;
    int16_t  last_out;

    emulator_t* emu;                 /* back-pointer (audio mixing hook)           */
} mea8000_t;

/* Lifecycle */
void mea8000_init(mea8000_t* m, uint16_t base_addr);
void mea8000_reset(mea8000_t* m);

/* CPU I/O (routed by io_bus for base_addr and base_addr+1) */
uint8_t mea8000_read(mea8000_t* m, uint16_t addr);
void    mea8000_write(mea8000_t* m, uint16_t addr, uint8_t value);

/* Advance the chip by `cycles` CPU cycles: runs the 64 kHz sequencer, updates
 * the ready/STATUS bit in CPU time, and fills the output ring. */
void mea8000_tick(mea8000_t* m, int cycles);

/* Pull `count` signed 16-bit samples at the emulator audio rate (resampled from
 * the 64 kHz ring), to be mixed with the PSG. Silence when idle. */
void mea8000_generate(mea8000_t* m, int16_t* out, int count);

/* True while a frame is being synthesised (for tests/inspection). */
bool mea8000_speaking(const mea8000_t* m);

#endif /* MEA8000_H */
