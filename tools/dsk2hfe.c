/**
 * @file dsk2hfe.c
 * @brief Convert an ORIC MFM_DISK .dsk to an HxC .HFE (v1) magnetic image.
 * @author bmarty <bmarty@mailo.com>
 *
 * Produces a bit-level MFM flux image (HFE v1, "HXCPICFE") that a HxC Floppy
 * Emulator, a Gotek running FlashFloppy, or a Greaseweazle can use to drive or
 * write a *real* Oric floppy — the disk equivalent of tap2wav for tapes.
 *
 * Input: the ORIC "MFM_DISK" container (Oricutron/Phosphoric):
 *   256-byte header ("MFM_DISK", u32 sides, u32 tracks, u32 geometry) then
 *   sides*tracks tracks of 6400 bytes each, ordered side0[all tracks] then
 *   side1[all tracks]. Each track is a byte-level ISO/IBM MFM track layout
 *   (0x4E gaps, 0x00 sync, C2C2C2FC index mark, A1A1A1FE id / A1A1A1FB data
 *   marks, sector data and CRC already present).
 *
 * Output: HFE v1 (spec: HxC "HFE file format"). Header at block 0, track LUT at
 * block 1, then per-track data. Sides are interleaved in 256-byte chunks; bits
 * are packed LSb-first; cell rate = 2 * bitRate. Each source byte becomes 16 MFM
 * cells (clock,data pairs); the A1 / C2 sync bytes that precede an address mark
 * are emitted with their standard missing-clock patterns 0x4489 / 0x5224.
 *
 * Usage: dsk2hfe IN.dsk OUT.hfe [--bitrate KBPS] [--rpm RPM]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MFM_HDR        256
#define MFM_TRACK      6400
#define HFE_BLOCK      512
#define SIDE_CELLS     (MFM_TRACK * 16)          /* 102400 cells / side       */
#define SIDE_BYTES     (SIDE_CELLS / 8)          /* 12800 HFE bytes / side    */

/* ── MFM cell emitter (LSb-first packing into a byte buffer) ─────────────── */
static void put_cell(uint8_t* buf, size_t* pos, int cell)
{
    if (cell) buf[*pos >> 3] |= (uint8_t)(1u << (*pos & 7));  /* LSb first */
    (*pos)++;
}

/* Encode a normal data byte: 8 (clock,data) cell pairs, MSB data first.
 * Clock cell is set only between two zero data bits (standard MFM rule). */
static void mfm_byte(uint8_t* buf, size_t* pos, int* prev, uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        int d = (b >> i) & 1;
        put_cell(buf, pos, (!*prev && !d) ? 1 : 0);   /* clock */
        put_cell(buf, pos, d);                        /* data  */
        *prev = d;
    }
}

/* Emit a raw 16-cell pattern (the missing-clock sync marks), MSB first. */
static void mfm_raw16(uint8_t* buf, size_t* pos, int* prev, uint16_t pat, int last_data)
{
    for (int i = 15; i >= 0; i--) put_cell(buf, pos, (pat >> i) & 1);
    *prev = last_data;
}

/* MFM-encode one 6400-byte track into SIDE_BYTES of HFE cell data. The A1/C2
 * bytes forming a sync triple before an address mark get the special encoding. */
static void encode_track(const uint8_t* trk, uint8_t* out)
{
    memset(out, 0, SIDE_BYTES);

    /* Mark which bytes are A1/C2 sync marks (part of a triple before a mark). */
    uint8_t* spec = (uint8_t*)calloc(1, MFM_TRACK);   /* 0=normal 1=A1 2=C2 */
    for (int i = 0; i + 3 < MFM_TRACK; i++) {
        if (trk[i] == 0xA1 && trk[i+1] == 0xA1 && trk[i+2] == 0xA1 &&
            (trk[i+3] == 0xFE || trk[i+3] == 0xFB || trk[i+3] == 0xFF)) {
            spec[i] = spec[i+1] = spec[i+2] = 1;
        } else if (trk[i] == 0xC2 && trk[i+1] == 0xC2 && trk[i+2] == 0xC2 &&
                   trk[i+3] == 0xFC) {
            spec[i] = spec[i+1] = spec[i+2] = 2;
        }
    }

    size_t pos = 0;
    int prev = 0;
    for (int i = 0; i < MFM_TRACK; i++) {
        if (spec[i] == 1)      mfm_raw16(out, &pos, &prev, 0x4489, 1); /* A1 */
        else if (spec[i] == 2) mfm_raw16(out, &pos, &prev, 0x5224, 0); /* C2 */
        else                   mfm_byte(out, &pos, &prev, trk[i]);
    }
    free(spec);
}

static void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

int main(int argc, char** argv)
{
    const char* in = NULL; const char* out = NULL;
    unsigned bitrate = 250, rpm = 300;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--bitrate") && i + 1 < argc) bitrate = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--rpm")     && i + 1 < argc) rpm     = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (argv[i][0] == '-') { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 2; }
        else if (!in) in = argv[i]; else if (!out) out = argv[i];
    }
    if (!in || !out) {
        fprintf(stderr, "Usage: %s IN.dsk OUT.hfe [--bitrate KBPS] [--rpm RPM]\n"
                        "Convert an ORIC MFM_DISK .dsk to an HxC .HFE (v1) magnetic image.\n", argv[0]);
        return 2;
    }

    FILE* f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "dsk2hfe: cannot open %s\n", in); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* raw = (uint8_t*)malloc((size_t)(sz > 0 ? sz : 1));
    if (!raw || sz <= 0 || fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "dsk2hfe: read error\n"); fclose(f); free(raw); return 1;
    }
    fclose(f);

    if (sz <= MFM_HDR || memcmp(raw, "MFM_DISK", 8) != 0) {
        fprintf(stderr, "dsk2hfe: not an MFM_DISK .dsk (need the raw-MFM Oric format).\n"
                        "         Convert a flat/Sedoric image to MFM_DISK first.\n");
        free(raw); return 1;
    }
    uint32_t sides  = raw[8]  | ((uint32_t)raw[9]  << 8) | ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
    uint32_t tracks = raw[12] | ((uint32_t)raw[13] << 8) | ((uint32_t)raw[14] << 16) | ((uint32_t)raw[15] << 24);
    if (sides < 1 || sides > 2 || tracks < 1 || tracks > 84) {
        fprintf(stderr, "dsk2hfe: implausible geometry (sides=%u tracks=%u)\n", sides, tracks);
        free(raw); return 1;
    }

    /* Track block: side0 and side1 interleaved in 256-byte chunks. */
    const uint32_t chunks    = SIDE_BYTES / 256;                 /* 50            */
    const uint32_t trk_bytes = chunks * HFE_BLOCK;               /* 25600 / track */
    const uint32_t trk_blocks = trk_bytes / HFE_BLOCK;           /* 50            */

    FILE* o = fopen(out, "wb");
    if (!o) { fprintf(stderr, "dsk2hfe: cannot write %s\n", out); free(raw); return 1; }

    /* ── Block 0: header ── */
    uint8_t hdr[HFE_BLOCK];
    memset(hdr, 0xFF, sizeof(hdr));
    memcpy(hdr, "HXCPICFE", 8);
    hdr[8]  = 0;                    /* format revision (HFE v1)     */
    hdr[9]  = (uint8_t)tracks;
    hdr[10] = (uint8_t)sides;
    hdr[11] = 0x00;                 /* ISO/IBM MFM encoding         */
    wr16(hdr + 12, (uint16_t)bitrate);
    wr16(hdr + 14, (uint16_t)rpm);
    hdr[16] = 0x00;                 /* IBM PC DD interface (Gotek/HxC-friendly) */
    hdr[17] = 1;                    /* dnu                          */
    wr16(hdr + 18, 1);             /* track list at block 1        */
    hdr[20] = 0xFF;                 /* write allowed                */
    fwrite(hdr, 1, HFE_BLOCK, o);

    /* ── Block 1: track offset LUT ── */
    uint8_t lut[HFE_BLOCK];
    memset(lut, 0xFF, sizeof(lut));
    for (uint32_t t = 0; t < tracks; t++) {
        uint16_t blk = (uint16_t)(2 + t * trk_blocks);   /* track data start block */
        wr16(lut + t * 4,     blk);
        wr16(lut + t * 4 + 2, (uint16_t)trk_bytes);      /* interleaved length     */
    }
    fwrite(lut, 1, HFE_BLOCK, o);

    /* ── Track data ── */
    uint8_t* s0 = (uint8_t*)malloc(SIDE_BYTES);
    uint8_t* s1 = (uint8_t*)malloc(SIDE_BYTES);
    uint8_t* blk = (uint8_t*)malloc(trk_bytes);
    for (uint32_t t = 0; t < tracks; t++) {
        uint32_t off0 = MFM_HDR + (0 * tracks + t) * MFM_TRACK;
        encode_track(raw + off0, s0);
        if (sides == 2) {
            uint32_t off1 = MFM_HDR + (1 * tracks + t) * MFM_TRACK;
            encode_track(raw + off1, s1);
        } else {
            memset(s1, 0x00, SIDE_BYTES);  /* single-sided: side 1 unused/filler */
        }
        /* Interleave 256-byte chunks: [s0 chunk][s1 chunk] per 512-block. */
        for (uint32_t k = 0; k < chunks; k++) {
            memcpy(blk + k * HFE_BLOCK,       s0 + k * 256, 256);
            memcpy(blk + k * HFE_BLOCK + 256, s1 + k * 256, 256);
        }
        fwrite(blk, 1, trk_bytes, o);
    }
    free(s0); free(s1); free(blk);
    fclose(o);

    printf("dsk2hfe: %s (%u side%s, %u tracks) -> %s  [HFE v1, %u kbit/s, %u RPM]\n",
           in, sides, sides > 1 ? "s" : "", tracks, out, bitrate, rpm);
    free(raw);
    return 0;
}
