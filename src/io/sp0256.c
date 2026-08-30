/**
 * @file sp0256.c
 * @brief Mageco "Synthétiseur Vocal" — GI SP0256-AL2 speech synthesizer core.
 * @author bmarty <bmarty@mailo.com>
 *
 * The SP0256 microsequencer, the 12-pole LPC lattice filter, the coefficient
 * quantization table (qtbl) and the microsequencer data-format tables
 * (sp0256_datafmt / sp0256_df_idx) below are a faithful C11 port of MAME's
 * src/devices/sound/sp0256.cpp:
 *
 *   license:BSD-3-Clause
 *   copyright-holders:Joseph Zbiciak, Tim Lindner
 *   "GI SP0256 Narrator Speech Processor — By Joe Zbiciak. Ported to MAME by
 *    tim lindner."
 *
 * The BSD-3-Clause licence permits reuse with attribution, retained here. The
 * common sp0256-al2.bin dump (md5 54db0ac2…) is stored bit-reversed per byte
 * relative to the microsequencer's logical bit order, so bitrev defaults ON
 * (sp->bitrev); MAME instead ships a pre-reversed ROM region and disables its
 * bitrevbuff. The toggle covers the other dump variant.
 *
 * Adaptation to Phosphoric: the chip is paced from CPU cycles (sp0256_tick,
 * ~100 cycles per 10 kHz sample) into an internal ring, then resampled to the
 * emulator audio rate (sp0256_generate) and mixed with the AY-3-8910 PSG. The
 * SPB-640 speech FIFO of the original is omitted (the Mageco board exposes a
 * single ALD port at $03F1).
 */

#include "io/sp0256.h"
#include "audio/audio.h"   /* AUDIO_SAMPLE_RATE */

#include <string.h>

/* ------------------------------------------------------------------------- */
/*  Equivalent timing periods (MAME).                                        */
/* ------------------------------------------------------------------------- */
#define PER_PAUSE    (64)
#define PER_NOISE    (64)

/* ======================================================================== */
/*  Data-format control-word packing (MAME CR macro).                        */
/*  len 4b | lshift 4b | param 4b | delta 1b | field 1b | clr5 1b | clrall 1b */
/* ======================================================================== */
#define CR(l,s,p,d,f,c5,ca)         \
        (                           \
            (((l)  & 15) <<  0) |   \
            (((s)  & 15) <<  4) |   \
            (((p)  & 15) <<  8) |   \
            (((d)  &  1) << 12) |   \
            (((f)  &  1) << 13) |   \
            (((c5) &  1) << 14) |   \
            (((ca) &  1) << 15)     \
        )

#define CR_DELTA  CR(0,0,0,1,0,0,0)
#define CR_FIELD  CR(0,0,0,0,1,0,0)
#define CR_CLR5   CR(0,0,0,0,0,1,0)
#define CR_CLRA   CR(0,0,0,0,0,0,1)
#define CR_LEN(x) ((x) & 15)
#define CR_SHF(x) (((x) >> 4) & 15)
#define CR_PRM(x) (((x) >> 8) & 15)

enum { AM = 0, PR, B0, F0, B1, F1, B2, F2, B3, F3, B4, F4, B5, F5, IA, IP };

/* ======================================================================== */
/*  qtbl -- Coefficient Quantization Table (from the SP0250 data sheet;      */
/*          correct for SP0256 per MAME).                                    */
/* ======================================================================== */
static const int16_t qtbl[128] =
{
    0,      9,      17,     25,     33,     41,     49,     57,
    65,     73,     81,     89,     97,     105,    113,    121,
    129,    137,    145,    153,    161,    169,    177,    185,
    193,    201,    209,    217,    225,    233,    241,    249,
    257,    265,    273,    281,    289,    297,    301,    305,
    309,    313,    317,    321,    325,    329,    333,    337,
    341,    345,    349,    353,    357,    361,    365,    369,
    373,    377,    381,    385,    389,    393,    397,    401,
    405,    409,    413,    417,    421,    425,    427,    429,
    431,    433,    435,    437,    439,    441,    443,    445,
    447,    449,    451,    453,    455,    457,    459,    461,
    463,    465,    467,    469,    471,    473,    475,    477,
    479,    481,    482,    483,    484,    485,    486,    487,
    488,    489,    490,    491,    492,    493,    494,    495,
    496,    497,    498,    499,    500,    501,    502,    503,
    504,    505,    506,    507,    508,    509,    510,    511
};

static const uint16_t sp0256_datafmt[] =
{
    /*  OPCODE 1111: PAUSE */
    /*    0 */  CR( 0,  0,  0,  0,  0,  0,  1),
    /*  Opcode 0001: LOADALL */
    /*    1 */  CR( 8,  0,  AM, 0,  0,  0,  1),
    /*    2 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*    3 */  CR( 8,  0,  B0, 0,  0,  0,  0),
    /*    4 */  CR( 8,  0,  F0, 0,  0,  0,  0),
    /*    5 */  CR( 8,  0,  B1, 0,  0,  0,  0),
    /*    6 */  CR( 8,  0,  F1, 0,  0,  0,  0),
    /*    7 */  CR( 8,  0,  B2, 0,  0,  0,  0),
    /*    8 */  CR( 8,  0,  F2, 0,  0,  0,  0),
    /*    9 */  CR( 8,  0,  B3, 0,  0,  0,  0),
    /*   10 */  CR( 8,  0,  F3, 0,  0,  0,  0),
    /*   11 */  CR( 8,  0,  B4, 0,  0,  0,  0),
    /*   12 */  CR( 8,  0,  F4, 0,  0,  0,  0),
    /*   13 */  CR( 8,  0,  B5, 0,  0,  0,  0),
    /*   14 */  CR( 8,  0,  F5, 0,  0,  0,  0),
    /*   15 */  CR( 8,  0,  IA, 0,  0,  0,  0),
    /*   16 */  CR( 8,  0,  IP, 0,  0,  0,  0),
    /*  Opcode 0100: LOAD_4 */
    /*   17 */  CR( 6,  2,  AM, 0,  0,  0,  1),
    /*   18 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*   19 */  CR( 4,  3,  B3, 0,  0,  0,  0),
    /*   20 */  CR( 6,  2,  F3, 0,  0,  0,  0),
    /*   21 */  CR( 7,  1,  B4, 0,  0,  0,  0),
    /*   22 */  CR( 6,  2,  F4, 0,  0,  0,  0),
    /*   23 */  CR( 8,  0,  B5, 0,  0,  0,  0),
    /*   24 */  CR( 8,  0,  F5, 0,  0,  0,  0),
    /*   25 */  CR( 6,  2,  AM, 0,  0,  0,  1),
    /*   26 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*   27 */  CR( 6,  1,  B3, 0,  0,  0,  0),
    /*   28 */  CR( 7,  1,  F3, 0,  0,  0,  0),
    /*   29 */  CR( 8,  0,  B4, 0,  0,  0,  0),
    /*   30 */  CR( 8,  0,  F4, 0,  0,  0,  0),
    /*   31 */  CR( 8,  0,  B5, 0,  0,  0,  0),
    /*   32 */  CR( 8,  0,  F5, 0,  0,  0,  0),
    /*  Opcode 0110: SETMSB_6 */
    /*   33 */  CR( 0,  0,  0,  0,  0,  1,  0),
    /*   34 */  CR( 6,  2,  AM, 0,  0,  0,  0),
    /*   35 */  CR( 6,  2,  F3, 0,  1,  0,  0),
    /*   36 */  CR( 6,  2,  F4, 0,  1,  0,  0),
    /*   37 */  CR( 8,  0,  F5, 0,  1,  0,  0),
    /*   38 */  CR( 0,  0,  0,  0,  0,  1,  0),
    /*   39 */  CR( 6,  2,  AM, 0,  0,  0,  0),
    /*   40 */  CR( 7,  1,  F3, 0,  1,  0,  0),
    /*   41 */  CR( 8,  0,  F4, 0,  1,  0,  0),
    /*   42 */  CR( 8,  0,  F5, 0,  1,  0,  0),
    /*   43 */  0,
    /*   44 */  0,
    /*  Opcode 1001: DELTA_9 */
    /*   45 */  CR( 4,  2,  AM, 1,  0,  0,  0),
    /*   46 */  CR( 5,  0,  PR, 1,  0,  0,  0),
    /*   47 */  CR( 3,  4,  B0, 1,  0,  0,  0),
    /*   48 */  CR( 3,  3,  F0, 1,  0,  0,  0),
    /*   49 */  CR( 3,  4,  B1, 1,  0,  0,  0),
    /*   50 */  CR( 3,  3,  F1, 1,  0,  0,  0),
    /*   51 */  CR( 3,  4,  B2, 1,  0,  0,  0),
    /*   52 */  CR( 3,  3,  F2, 1,  0,  0,  0),
    /*   53 */  CR( 3,  3,  B3, 1,  0,  0,  0),
    /*   54 */  CR( 4,  2,  F3, 1,  0,  0,  0),
    /*   55 */  CR( 4,  1,  B4, 1,  0,  0,  0),
    /*   56 */  CR( 4,  2,  F4, 1,  0,  0,  0),
    /*   57 */  CR( 5,  0,  B5, 1,  0,  0,  0),
    /*   58 */  CR( 5,  0,  F5, 1,  0,  0,  0),
    /*   59 */  CR( 4,  2,  AM, 1,  0,  0,  0),
    /*   60 */  CR( 5,  0,  PR, 1,  0,  0,  0),
    /*   61 */  CR( 4,  1,  B0, 1,  0,  0,  0),
    /*   62 */  CR( 4,  2,  F0, 1,  0,  0,  0),
    /*   63 */  CR( 4,  1,  B1, 1,  0,  0,  0),
    /*   64 */  CR( 4,  2,  F1, 1,  0,  0,  0),
    /*   65 */  CR( 4,  1,  B2, 1,  0,  0,  0),
    /*   66 */  CR( 4,  2,  F2, 1,  0,  0,  0),
    /*   67 */  CR( 4,  1,  B3, 1,  0,  0,  0),
    /*   68 */  CR( 5,  1,  F3, 1,  0,  0,  0),
    /*   69 */  CR( 5,  0,  B4, 1,  0,  0,  0),
    /*   70 */  CR( 5,  0,  F4, 1,  0,  0,  0),
    /*   71 */  CR( 5,  0,  B5, 1,  0,  0,  0),
    /*   72 */  CR( 5,  0,  F5, 1,  0,  0,  0),
    /*  Opcode 1010: SETMSB_A */
    /*   73 */  CR( 0,  0,  0,  0,  0,  1,  0),
    /*   74 */  CR( 6,  2,  AM, 0,  0,  0,  0),
    /*   75 */  CR( 5,  3,  F0, 0,  1,  0,  0),
    /*   76 */  CR( 5,  3,  F1, 0,  1,  0,  0),
    /*   77 */  CR( 5,  3,  F2, 0,  1,  0,  0),
    /*   78 */  CR( 0,  0,  0,  0,  0,  1,  0),
    /*   79 */  CR( 6,  2,  AM, 0,  0,  0,  0),
    /*   80 */  CR( 6,  2,  F0, 0,  1,  0,  0),
    /*   81 */  CR( 6,  2,  F1, 0,  1,  0,  0),
    /*   82 */  CR( 6,  2,  F2, 0,  1,  0,  0),
    /*  Opcode 0010: LOAD_2 / Opcode 1100: LOAD_C  (Mode 00 and 10) */
    /*   83 */  CR( 6,  2,  AM, 0,  0,  0,  1),
    /*   84 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*   85 */  CR( 3,  4,  B0, 0,  0,  0,  0),
    /*   86 */  CR( 5,  3,  F0, 0,  0,  0,  0),
    /*   87 */  CR( 3,  4,  B1, 0,  0,  0,  0),
    /*   88 */  CR( 5,  3,  F1, 0,  0,  0,  0),
    /*   89 */  CR( 3,  4,  B2, 0,  0,  0,  0),
    /*   90 */  CR( 5,  3,  F2, 0,  0,  0,  0),
    /*   91 */  CR( 4,  3,  B3, 0,  0,  0,  0),
    /*   92 */  CR( 6,  2,  F3, 0,  0,  0,  0),
    /*   93 */  CR( 7,  1,  B4, 0,  0,  0,  0),
    /*   94 */  CR( 6,  2,  F4, 0,  0,  0,  0),
    /*   95 */  CR( 5,  0,  IA, 0,  0,  0,  0),
    /*   96 */  CR( 5,  0,  IP, 0,  0,  0,  0),
    /*   97 */  CR( 6,  2,  AM, 0,  0,  0,  1),
    /*   98 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*   99 */  CR( 6,  1,  B0, 0,  0,  0,  0),
    /*  100 */  CR( 6,  2,  F0, 0,  0,  0,  0),
    /*  101 */  CR( 6,  1,  B1, 0,  0,  0,  0),
    /*  102 */  CR( 6,  2,  F1, 0,  0,  0,  0),
    /*  103 */  CR( 6,  1,  B2, 0,  0,  0,  0),
    /*  104 */  CR( 6,  2,  F2, 0,  0,  0,  0),
    /*  105 */  CR( 6,  1,  B3, 0,  0,  0,  0),
    /*  106 */  CR( 7,  1,  F3, 0,  0,  0,  0),
    /*  107 */  CR( 8,  0,  B4, 0,  0,  0,  0),
    /*  108 */  CR( 8,  0,  F4, 0,  0,  0,  0),
    /*  109 */  CR( 5,  0,  IA, 0,  0,  0,  0),
    /*  110 */  CR( 5,  0,  IP, 0,  0,  0,  0),
    /*  OPCODE 1101: DELTA_D */
    /*  111 */  CR( 4,  2,  AM, 1,  0,  0,  0),
    /*  112 */  CR( 5,  0,  PR, 1,  0,  0,  0),
    /*  113 */  CR( 3,  3,  B3, 1,  0,  0,  0),
    /*  114 */  CR( 4,  2,  F3, 1,  0,  0,  0),
    /*  115 */  CR( 4,  1,  B4, 1,  0,  0,  0),
    /*  116 */  CR( 4,  2,  F4, 1,  0,  0,  0),
    /*  117 */  CR( 5,  0,  B5, 1,  0,  0,  0),
    /*  118 */  CR( 5,  0,  F5, 1,  0,  0,  0),
    /*  119 */  CR( 4,  2,  AM, 1,  0,  0,  0),
    /*  120 */  CR( 5,  0,  PR, 1,  0,  0,  0),
    /*  121 */  CR( 4,  1,  B3, 1,  0,  0,  0),
    /*  122 */  CR( 5,  1,  F3, 1,  0,  0,  0),
    /*  123 */  CR( 5,  0,  B4, 1,  0,  0,  0),
    /*  124 */  CR( 5,  0,  F4, 1,  0,  0,  0),
    /*  125 */  CR( 5,  0,  B5, 1,  0,  0,  0),
    /*  126 */  CR( 5,  0,  F5, 1,  0,  0,  0),
    /*  OPCODE 1110: LOAD_E */
    /*  127 */  CR( 6,  2,  AM, 0,  0,  0,  0),
    /*  128 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*  Opcode 0010: LOAD_2 / Opcode 1100: LOAD_C  (Mode 01 and 11) */
    /*  129 */  CR( 6,  2,  AM, 0,  0,  0,  1),
    /*  130 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*  131 */  CR( 3,  4,  B0, 0,  0,  0,  0),
    /*  132 */  CR( 5,  3,  F0, 0,  0,  0,  0),
    /*  133 */  CR( 3,  4,  B1, 0,  0,  0,  0),
    /*  134 */  CR( 5,  3,  F1, 0,  0,  0,  0),
    /*  135 */  CR( 3,  4,  B2, 0,  0,  0,  0),
    /*  136 */  CR( 5,  3,  F2, 0,  0,  0,  0),
    /*  137 */  CR( 4,  3,  B3, 0,  0,  0,  0),
    /*  138 */  CR( 6,  2,  F3, 0,  0,  0,  0),
    /*  139 */  CR( 7,  1,  B4, 0,  0,  0,  0),
    /*  140 */  CR( 6,  2,  F4, 0,  0,  0,  0),
    /*  141 */  CR( 8,  0,  B5, 0,  0,  0,  0),
    /*  142 */  CR( 8,  0,  F5, 0,  0,  0,  0),
    /*  143 */  CR( 5,  0,  IA, 0,  0,  0,  0),
    /*  144 */  CR( 5,  0,  IP, 0,  0,  0,  0),
    /*  145 */  CR( 6,  2,  AM, 0,  0,  0,  1),
    /*  146 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*  147 */  CR( 6,  1,  B0, 0,  0,  0,  0),
    /*  148 */  CR( 6,  2,  F0, 0,  0,  0,  0),
    /*  149 */  CR( 6,  1,  B1, 0,  0,  0,  0),
    /*  150 */  CR( 6,  2,  F1, 0,  0,  0,  0),
    /*  151 */  CR( 6,  1,  B2, 0,  0,  0,  0),
    /*  152 */  CR( 6,  2,  F2, 0,  0,  0,  0),
    /*  153 */  CR( 6,  1,  B3, 0,  0,  0,  0),
    /*  154 */  CR( 7,  1,  F3, 0,  0,  0,  0),
    /*  155 */  CR( 8,  0,  B4, 0,  0,  0,  0),
    /*  156 */  CR( 8,  0,  F4, 0,  0,  0,  0),
    /*  157 */  CR( 8,  0,  B5, 0,  0,  0,  0),
    /*  158 */  CR( 8,  0,  F5, 0,  0,  0,  0),
    /*  159 */  CR( 5,  0,  IA, 0,  0,  0,  0),
    /*  160 */  CR( 5,  0,  IP, 0,  0,  0,  0),
    /*  Opcode 0011: SETMSB_3 / Opcode 0101: SETMSB_5 */
    /*  161 */  CR( 0,  0,  0,  0,  0,  1,  0),
    /*  162 */  CR( 6,  2,  AM, 0,  0,  0,  0),
    /*  163 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*  164 */  CR( 5,  3,  F0, 0,  1,  0,  0),
    /*  165 */  CR( 5,  3,  F1, 0,  1,  0,  0),
    /*  166 */  CR( 5,  3,  F2, 0,  1,  0,  0),
    /*  167 */  CR( 5,  0,  IA, 0,  0,  0,  0),
    /*  168 */  CR( 5,  0,  IP, 0,  0,  0,  0),
    /*  169 */  CR( 0,  0,  0,  0,  0,  1,  0),
    /*  170 */  CR( 6,  2,  AM, 0,  0,  0,  0),
    /*  171 */  CR( 8,  0,  PR, 0,  0,  0,  0),
    /*  172 */  CR( 6,  2,  F0, 0,  1,  0,  0),
    /*  173 */  CR( 6,  2,  F1, 0,  1,  0,  0),
    /*  174 */  CR( 6,  2,  F2, 0,  1,  0,  0),
    /*  175 */  CR( 5,  0,  IA, 0,  0,  0,  0),
    /*  176 */  CR( 5,  0,  IP, 0,  0,  0,  0),
};

static const int16_t sp0256_df_idx[16 * 8] =
{
    /*  OPCODE 0000 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 1000 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 0100 */      17, 22,     17, 24,     25, 30,     25, 32,
    /*  OPCODE 1100 */      83, 94,     129,142,    97, 108,    145,158,
    /*  OPCODE 0010 */      83, 96,     129,144,    97, 110,    145,160,
    /*  OPCODE 1010 */      73, 77,     74, 77,     78, 82,     79, 82,
    /*  OPCODE 0110 */      33, 36,     34, 37,     38, 41,     39, 42,
    /*  OPCODE 1110 */      127,128,    127,128,    127,128,    127,128,
    /*  OPCODE 0001 */      1,  14,     1,  16,     1,  14,     1,  16,
    /*  OPCODE 1001 */      45, 56,     45, 58,     59, 70,     59, 72,
    /*  OPCODE 0101 */      161,166,    162,166,    169,174,    170,174,
    /*  OPCODE 1101 */      111,116,    111,118,    119,124,    119,126,
    /*  OPCODE 0011 */      161,168,    162,168,    169,176,    170,176,
    /*  OPCODE 1011 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 0111 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 1111 */      0,  0,      0,  0,      0,  0,      0,  0
};

/* ======================================================================== */
/*  Bit helpers (MAME).                                                      */
/* ======================================================================== */
static inline uint32_t bitrev32(uint32_t val)
{
    val = ((val & 0xFFFF0000u) >> 16) | ((val & 0x0000FFFFu) << 16);
    val = ((val & 0xFF00FF00u) >>  8) | ((val & 0x00FF00FFu) <<  8);
    val = ((val & 0xF0F0F0F0u) >>  4) | ((val & 0x0F0F0F0Fu) <<  4);
    val = ((val & 0xCCCCCCCCu) >>  2) | ((val & 0x33333333u) <<  2);
    val = ((val & 0xAAAAAAAAu) >>  1) | ((val & 0x55555555u) <<  1);
    return val;
}

static inline uint8_t bitrev8(uint8_t val)
{
    val = (uint8_t)(((val & 0xF0) >> 4) | ((val & 0x0F) << 4));
    val = (uint8_t)(((val & 0xCC) >> 2) | ((val & 0x33) << 2));
    val = (uint8_t)(((val & 0xAA) >> 1) | ((val & 0x55) << 1));
    return val;
}

/* ROM byte at a *logical* SP0256 byte address ($1000-$17FF map to al2.bin). */
static inline uint8_t sp0256_rombyte(sp0256_t* sp, int addr)
{
    uint8_t b = 0;
    if (addr >= 0x1000 && addr < 0x1000 + SP0256_ROM_SIZE)
        b = sp->rom[addr - 0x1000];
    return sp->bitrev ? bitrev8(b) : b;
}

/* ======================================================================== */
/*  getb -- Get up to 8 bits at the current PC (ROM path only; no FIFO).     */
/* ======================================================================== */
static uint32_t sp0256_getb(sp0256_t* sp, int len)
{
    int idx0 = (sp->pc) >> 3;
    int idx1 = (sp->pc + 8) >> 3;
    uint32_t d0 = sp0256_rombyte(sp, idx0);
    uint32_t d1 = sp0256_rombyte(sp, idx1);
    uint32_t data = ((d1 << 8) | d0) >> (sp->pc & 7);
    sp->pc += len;
    data &= ((1u << len) - 1u);
    return data;
}

/* ======================================================================== */
/*  regdec -- Decode the register set in the filter bank.                    */
/* ======================================================================== */
static inline int16_t sp0256_iq(uint8_t x)
{
    int xi = (int)x;
    if (x & 0x80) return qtbl[0x7F & (-xi)];
    return (int16_t)(-qtbl[xi & 0x7F]);
}

static void sp0256_regdec(sp0256_lpc12_t* f)
{
    f->amp = (f->r[0] & 0x1F) << (((f->r[0] & 0xE0) >> 5) + 0);
    f->cnt = 0;
    f->per = f->r[1];
    for (int i = 0; i < 6; i++) {
        f->b_coef[i] = sp0256_iq(f->r[2 + 2 * i]);
        f->f_coef[i] = sp0256_iq(f->r[3 + 2 * i]);
    }
    f->interp = f->r[14] || f->r[15];
}

/* ======================================================================== */
/*  limit -- Digital sample output limiter (HIGH_QUALITY variant).          */
/* ======================================================================== */
static inline int16_t sp0256_limit(int16_t s)
{
    if (s >  8191) return  8191;
    if (s < -8192) return -8192;
    return s;
}

/* ======================================================================== */
/*  lpc12_update -- Update the 12-pole filter, outputting samples to ring.   */
/*  Returns the number of samples produced (may be < num_samp on rpt expiry).*/
/* ======================================================================== */
static int sp0256_lpc12_update(sp0256_lpc12_t* f, int num_samp,
                               int16_t* out, uint32_t* optr)
{
    int i;
    uint32_t oidx = *optr;

    for (i = 0; i < num_samp; i++) {
        int do_int = 0;
        uint16_t samp = 0;

        if (f->per) {
            if (f->cnt <= 0) {
                f->cnt += f->per;
                samp = (uint16_t)f->amp;
                f->rpt--;
                do_int = f->interp;
                for (int j = 0; j < 6; j++)
                    f->z_data[j][1] = f->z_data[j][0] = 0;
            } else {
                samp = 0;
                f->cnt--;
            }
        } else {
            if (--f->cnt <= 0) {
                do_int = f->interp;
                f->cnt = PER_NOISE;
                f->rpt--;
                for (int j = 0; j < 6; j++)
                    f->z_data[j][0] = f->z_data[j][1] = 0;
            }
            int bit = f->rng & 1;
            f->rng = (f->rng >> 1) ^ (bit ? 0x4001 : 0);
            samp = bit ? (uint16_t)f->amp : (uint16_t)(-f->amp);
        }

        if (do_int) {
            f->r[0] = (uint8_t)(f->r[0] + f->r[14]);
            f->r[1] = (uint8_t)(f->r[1] + f->r[15]);
            f->amp = (f->r[0] & 0x1F) << (((f->r[0] & 0xE0) >> 5) + 0);
            f->per = f->r[1];
        }

        if (f->rpt <= 0)
            break;

        for (int j = 0; j < 6; j++) {
            samp = (uint16_t)(samp + (((int)f->b_coef[j] * (int)f->z_data[j][1]) >> 9));
            samp = (uint16_t)(samp + (((int)f->f_coef[j] * (int)f->z_data[j][0]) >> 8));
            f->z_data[j][1] = f->z_data[j][0];
            f->z_data[j][0] = (int16_t)samp;
        }

        out[oidx++ & SP0256_SCBUF_MASK] = (int16_t)(sp0256_limit((int16_t)samp) << 2);
    }

    *optr = oidx;
    return i;
}

/* ======================================================================== */
/*  micro -- Emulate the microsequencer (no SPB-640 FIFO).                   */
/* ======================================================================== */
static void sp0256_micro(sp0256_t* sp)
{
    uint8_t immed4, opcode;
    uint16_t cr;
    int ctrl_xfer, repeat, i, idx0, idx1;

    while (sp->filt.rpt <= 0) {
        /* Pick up a pending ALD if halted. */
        if (sp->halted && !sp->lrq) {
            sp->pc     = sp->ald | (0x1000 << 3);
            sp->halted = 0;
            sp->lrq    = 1;
            sp->ald    = 0;
            for (i = 0; i < 16; i++) sp->filt.r[i] = 0;
        }

        /* Still halted → idle. */
        if (sp->halted) {
            sp->filt.rpt = 1;
            sp->lrq      = 1;
            sp->ald      = 0;
            for (i = 0; i < 16; i++) sp->filt.r[i] = 0;
            sp->sby = 1;
            return;
        }

        immed4    = (uint8_t)sp0256_getb(sp, 4);
        opcode    = (uint8_t)sp0256_getb(sp, 4);
        repeat    = 0;
        ctrl_xfer = 0;

        switch (opcode) {
            case 0x0: /* RTS / SETPAGE */
                if (immed4) {
                    sp->page = bitrev32(immed4) >> 13;
                } else {
                    uint32_t btrg = (uint32_t)sp->stack;
                    sp->stack = 0;
                    if (!btrg) { sp->halted = 1; sp->pc = 0; ctrl_xfer = 1; }
                    else       { sp->pc = (int)btrg; ctrl_xfer = 1; }
                }
                break;

            case 0xE: /* JMP */
            case 0xD: /* JSR */
            {
                uint32_t btrg = (uint32_t)sp->page
                              | (bitrev32(immed4) >> 17)
                              | (bitrev32(sp0256_getb(sp, 8)) >> 21);
                ctrl_xfer = 1;
                if (opcode == 0xD)
                    sp->stack = (sp->pc + 7) & ~7;
                sp->pc = (int)btrg;
                break;
            }

            case 0x1: /* SETMODE */
                sp->mode = ((immed4 & 8) >> 2) | (immed4 & 4) | ((immed4 & 3) << 4);
                break;

            default:
                repeat = immed4 | (sp->mode & 0x30);
                break;
        }
        if (opcode != 1) sp->mode &= 0xF;

        if (ctrl_xfer) {
            /* No FIFO on the Mageco board: control transfers just set the PC. */
            continue;
        }

        if (!repeat) continue;

        sp->filt.rpt = repeat + 1;

        i    = (opcode << 3) | (sp->mode & 6);
        idx0 = sp0256_df_idx[i++];
        idx1 = sp0256_df_idx[i];

        if (idx0 < 0 || idx1 < 0 || idx1 < idx0)
            continue; /* undefined opcode/mode combination */

        for (i = idx0; i <= idx1; i++) {
            int len, shf, delta, field, prm, clra, clr5;
            int8_t value;

            cr    = sp0256_datafmt[i];
            len   = CR_LEN(cr);
            shf   = CR_SHF(cr);
            prm   = CR_PRM(cr);
            clra  = cr & CR_CLRA;
            clr5  = cr & CR_CLR5;
            delta = cr & CR_DELTA;
            field = cr & CR_FIELD;
            value = 0;

            if (clra) {
                for (int j = 0; j < 16; j++) sp->filt.r[j] = 0;
                sp->silent = 1;
            }
            if (clr5)
                sp->filt.r[B5] = sp->filt.r[F5] = 0;

            if (len)
                value = (int8_t)sp0256_getb(sp, len);
            else
                continue;

            if (delta) {
                /* Sign-extend the len-bit field (avoids shifting a negative). */
                if (value & (1 << (len - 1)))
                    value = (int8_t)(value | (int8_t)(0xFFu << len));
            }
            if (shf)
                value = (int8_t)(value << shf);

            sp->silent = 0;

            if (field) {
                sp->filt.r[prm] &= (uint8_t)~(~0u << shf);
                sp->filt.r[prm] |= (uint8_t)value;
                continue;
            }
            if (delta) {
                sp->filt.r[prm] = (uint8_t)(sp->filt.r[prm] + value);
                continue;
            }
            sp->filt.r[prm] = (uint8_t)value;
        }

        if (opcode == 0xF) {
            sp->silent = 1;
            sp->filt.r[1] = PER_PAUSE;
        }

        sp0256_regdec(&sp->filt);
        break;
    }
}

/* ======================================================================== */
/*  Sample production: fill the ring with `want` 10 kHz samples.            */
/* ======================================================================== */
static void sp0256_produce(sp0256_t* sp, int want)
{
    int did = 0;
    while (did < want) {
        if (sp->filt.rpt <= 0)
            sp0256_micro(sp);

        int room = want - did;
        if (sp->silent && sp->filt.rpt <= 0) {
            for (int x = 0; x < room; x++)
                sp->scratch[sp->sc_head++ & SP0256_SCBUF_MASK] = 0;
            did += room;
        } else {
            int n = sp0256_lpc12_update(&sp->filt, room, sp->scratch, &sp->sc_head);
            did += (n > 0) ? n : 0;
            if (n == 0 && sp->filt.rpt <= 0) {
                /* filter idle but not flagged silent: emit a sample to progress */
                sp->scratch[sp->sc_head++ & SP0256_SCBUF_MASK] = 0;
                did += 1;
            }
        }
    }

    /* Keep only the most recent ring window (lossy if audio isn't draining). */
    uint32_t occ = sp->sc_head - sp->sc_tail;
    if (occ > SP0256_SCBUF_SIZE)
        sp->sc_tail = sp->sc_head - SP0256_SCBUF_SIZE;
}

/* ======================================================================== */
/*  Public API                                                              */
/* ======================================================================== */
void sp0256_reset(sp0256_t* sp)
{
    memset(&sp->filt, 0, sizeof(sp->filt));
    sp->halted   = 1;
    sp->filt.rpt = -1;
    sp->filt.rng = 1;
    sp->lrq      = 1;
    sp->ald      = 0;
    sp->pc       = 0;
    sp->stack    = 0;
    sp->mode     = 0;
    sp->page     = 0x1000 << 3;
    sp->silent   = 1;
    sp->sby      = 1;
    sp->sc_head  = sp->sc_tail = 0;
    sp->cycle_acc = 0;
    sp->resample_acc = 0;
    sp->last_sample  = 0;
}

void sp0256_init(sp0256_t* sp, uint16_t base_addr)
{
    memset(sp, 0, sizeof(*sp));
    sp->base_addr = base_addr ? base_addr : SP0256_BASE_DEFAULT;
    /* The common sp0256-al2.bin dump (md5 54db0ac2…) is stored bit-reversed per
     * byte relative to the microsequencer's logical bit order: enabling bitrev
     * makes $1000 decode to "JMP @@PA1" (verified) and every allophone terminate
     * with a clean RTS→HALT at realistic durations. (MAME instead ships a
     * pre-reversed ROM region, hence its bitrevbuff is disabled.) */
    sp->bitrev    = true;
    sp->rom_valid = false;
    sp0256_reset(sp);
}

bool sp0256_load_rom(sp0256_t* sp, const uint8_t* data, uint32_t size)
{
    if (!data || size != SP0256_ROM_SIZE)
        return false;
    memcpy(sp->rom, data, SP0256_ROM_SIZE);
    sp->rom_valid = true;
    return true;
}

void sp0256_write(sp0256_t* sp, uint16_t addr, uint8_t value)
{
    if (addr != sp->base_addr) return;
    /* ALD write. Drop if the chip is still busy (LRQ low) — Frelon polls LRQ and
     * only writes when ready, so drops don't occur in practice. The 6-bit ALD
     * latches A1-A6; Frelon's first write is $80 (bit 7 set, not a valid 0-63
     * allophone) → masked to 0 (a ~10 ms @@PA1 pause, inaudible), which the game
     * uses as an init strobe. */
    if (!sp->lrq) return;
    sp->lrq = 0;
    sp->ald = (int)(value & 0x3F) << 4;  /* 6-bit allophone → 2-byte jump entry */
    sp->sby = 0;
}

uint8_t sp0256_read(sp0256_t* sp, uint16_t addr)
{
    if (addr != sp->base_addr) return 0xFF;
    /* Polarity CONFIRMED in-game against Frelon: it polls this register before
     * each allophone; with LRQ on bit 7 (1 = ready) Frelon speaks its full
     * utterance (32 allophones), whereas inverting the polarity freezes it after
     * the 2nd write — proving both that Frelon polls and that this mapping is
     * correct for the Mageco board. */
    uint8_t s = 0;
    if (sp->lrq) s |= SP0256_STAT_LRQ;   /* ready for next allophone */
    if (sp->sby) s |= SP0256_STAT_SBY;   /* all speech finished      */
    return s;
}

void sp0256_tick(sp0256_t* sp, int cycles)
{
    if (!sp->rom_valid || cycles <= 0) return;
    sp->cycle_acc += cycles;
    const int per = 1000000 / SP0256_SAMPLE_RATE;   /* 100 CPU cycles / sample */
    int nsamp = sp->cycle_acc / per;
    if (nsamp <= 0) return;
    sp->cycle_acc -= nsamp * per;
    sp0256_produce(sp, nsamp);
}

void sp0256_generate(sp0256_t* sp, int16_t* out, int count)
{
    if (!sp->rom_valid) {
        memset(out, 0, (size_t)count * sizeof(int16_t));
        return;
    }
    for (int i = 0; i < count; i++) {
        out[i] = sp->last_sample;
        sp->resample_acc += SP0256_SAMPLE_RATE;         /* 10000 */
        while (sp->resample_acc >= AUDIO_SAMPLE_RATE) {  /* 44100 */
            sp->resample_acc -= AUDIO_SAMPLE_RATE;
            if (sp->sc_tail != sp->sc_head)
                sp->last_sample = sp->scratch[sp->sc_tail++ & SP0256_SCBUF_MASK];
            else
                sp->last_sample = 0;   /* underrun → silence */
        }
    }
}

bool sp0256_speaking(const sp0256_t* sp)
{
    return sp->rom_valid && !sp->sby;
}
