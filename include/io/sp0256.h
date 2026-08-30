/**
 * @file sp0256.h
 * @brief Mageco "Synthétiseur Vocal" — GI SP0256-AL2 speech synthesizer for ORIC
 * @author bmarty <bmarty@mailo.com>
 *
 * Mageco Electronic "Periph'Oric" speech box: a General Instrument SP0256-AL2
 * allophone speech chip on the ORIC expansion bus. Driver software by
 * M. Tortosa (cassette); used by games such as Cobra Pinball (Cobra Soft) and
 * Frelon.
 *
 * I/O address CONFIRMED two ways: empirical trace of Frelon (writes to $03F1
 * from its speech routine) + PassionOric reference ("03x1"). Base configurable
 * (--sp0256-addr), default $03F1.
 *
 * The SP0256 uses Linear Predictive Coding (LPC): a 12-pole lattice filter
 * driven by periodic impulses (voiced) or pseudo-random noise (unvoiced),
 * replaying 64 allophones stored as bit-packed LPC microcode in the chip's
 * internal 2 KB mask ROM (sp0256-al2.bin, loaded via --sp0256-rom). Output is
 * mixed into the emulator audio alongside the AY-3-8910 PSG.
 *
 * The synthesis core (microsequencer opcode decoder, LPC-12 lattice filter,
 * coefficient quantization table, data-format tables) is a faithful C11 port of
 * MAME's src/devices/sound/sp0256.cpp — license BSD-3-Clause, copyright Joseph
 * Zbiciak and Tim Lindner. That permissive licence allows verbatim reuse with
 * attribution (retained in sp0256.c). Joe Zbiciak also authored the SP0256-AL2
 * ROM reverse-engineering that documents the same bitfield layout.
 *
 * Register interface (single port at the base address, Mageco board):
 *   write : ALD — load a 6-bit allophone number (0-63) → the chip speaks it.
 *   read  : status — the SP0256 LRQ (Load ReQuest, ready for next allophone) is
 *           exposed on bit 7 and SBY (Standby, all speech finished) on bit 6.
 *           CONFIRMED in-game: Frelon polls this register and speaks its full
 *           utterance with this mapping, but freezes if the polarity is inverted
 *           (see the .c note in sp0256_read).
 */

#ifndef SP0256_H
#define SP0256_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration for emulator (avoids circular include) */
typedef struct emulator_s emulator_t;

#define SP0256_BASE_DEFAULT   0x03F1   /* Mageco board, confirmed vs Frelon      */

#define SP0256_ROM_SIZE       2048     /* SP0256-AL2 internal mask ROM (2 KB)    */
#define SP0256_ALLOPHONES     64       /* 6-bit allophone address space          */

/* SP0256-AL2 nominal internal sample rate: 3.12 MHz crystal / (6*4*13) = 10 kHz.
 * Samples are produced at this rate then resampled to the emulator audio rate. */
#define SP0256_SAMPLE_RATE    10000

/* Status bits returned on a read of the port (convention; calibrated vs Frelon). */
#define SP0256_STAT_LRQ       0x80     /* Load Request : ready for next ALD       */
#define SP0256_STAT_SBY       0x40     /* Standby : all speech finished           */

/* Internal scratch ring for generated 10 kHz samples (power of two). */
#define SP0256_SCBUF_SIZE     4096
#define SP0256_SCBUF_MASK     (SP0256_SCBUF_SIZE - 1)

/* ── LPC-12 lattice filter bank (MAME lpc12_t) ────────────────────────────── */
typedef struct sp0256_lpc12_s {
    int      rpt, cnt;         /* Repeat counter, interp counter                 */
    int      per, rng;         /* Period, noise LFSR                             */
    int      amp;              /* Amplitude                                      */
    int16_t  f_coef[6];        /* F coefficients (resonators)                    */
    int16_t  b_coef[6];        /* B coefficients                                 */
    int16_t  z_data[6][2];     /* Filter state (2 per stage)                     */
    uint8_t  r[16];            /* Raw parameter registers                        */
    int      interp;           /* Interpolation enabled                          */
} sp0256_lpc12_t;

typedef struct sp0256_s {
    uint16_t base_addr;                /* I/O base ($03F1 by default)            */

    /* Allophone ROM (sp0256-al2.bin), loaded via --sp0256-rom. Addressed by the
     * microsequencer at logical bytes $1000-$17FF (page $1000). */
    uint8_t  rom[SP0256_ROM_SIZE];
    bool     rom_valid;
    bool     bitrev;                   /* ROM stored bit-reversed per byte?      */

    /* ── Microsequencer state (MAME) ─────────────────────────────────────── */
    int      halted;                   /* CPU halted (idle)                      */
    int      lrq;                      /* Load ReQuest (1 = ready for next ALD)  */
    int      sby;                      /* Standby line (1 = all speech done)     */
    int      ald;                      /* Address LoaD latch (bit address)       */
    int      pc;                       /* Program counter (bit address)          */
    int      stack;                    /* Return address (bit address)           */
    int      mode;                     /* Current mode / repeat MSBs             */
    int      page;                     /* Current page (bit address)             */
    int      silent;                   /* Current frame is silent                */

    sp0256_lpc12_t filt;               /* LPC-12 filter bank                     */

    /* Generated-sample ring (10 kHz) */
    int16_t  scratch[SP0256_SCBUF_SIZE];
    uint32_t sc_head, sc_tail;

    /* CPU-cycle → 10 kHz sample pacing (driven from sp0256_tick). */
    int32_t  cycle_acc;                /* leftover CPU cycles                    */

    /* 10 kHz → emulator-rate resampling accumulator (used by sp0256_generate). */
    uint32_t resample_acc;
    int16_t  last_sample;

    emulator_t* emu;                   /* back-pointer (audio mixing hook)       */
} sp0256_t;

/* Lifecycle */
void sp0256_init(sp0256_t* sp, uint16_t base_addr);
void sp0256_reset(sp0256_t* sp);
bool sp0256_load_rom(sp0256_t* sp, const uint8_t* data, uint32_t size);

/* CPU I/O (routed by io_bus for the base address) */
uint8_t sp0256_read(sp0256_t* sp, uint16_t addr);
void    sp0256_write(sp0256_t* sp, uint16_t addr, uint8_t value);

/* Advance the chip by `cycles` CPU cycles: produces 10 kHz samples into the
 * internal ring and updates the LRQ/SBY status in CPU time (so a polling loop
 * observes progress even with no audio sink). Called from io_bus_tick(). */
void sp0256_tick(sp0256_t* sp, int cycles);

/* Pull `count` signed 16-bit samples at the emulator audio rate (resampled from
 * the 10 kHz ring), to be mixed with the PSG output. Silence when idle. */
void sp0256_generate(sp0256_t* sp, int16_t* out, int count);

/* True while speech is being produced (SBY low) — for tests/inspection. */
bool sp0256_speaking(const sp0256_t* sp);

#endif /* SP0256_H */
