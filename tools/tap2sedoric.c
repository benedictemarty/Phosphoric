/**
 * @file tap2sedoric.c
 * @brief TAP to Sedoric disk converter -- injects an Oric .tap file into a copy
 *        of a Sedoric (.dsk, MFM_DISK) disk image so it appears in the DIR.
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-15
 * @version 1.0.0
 *
 * Self-contained (no Phosphoric libs): build with
 *     cc -O2 -o tap2sedoric tap2sedoric.c
 *
 * Usage:
 *     tap2sedoric <input.tap> -o <output.dsk> -b <base.dsk> [-n NAME.EXT]
 *
 * - <base.dsk> must be a Sedoric MFM_DISK image (e.g. a blank formatted V4 disk).
 * - The downloaded file (from the Oric terminal's CSAVE .tap) is written into
 *   free sectors as a Sedoric file: a descriptor sector (load/end address +
 *   sector map) followed by the data sectors, plus a directory entry and the
 *   VTOC counters updated. Verified by booting Sedoric and running DIR.
 *
 * On-disk Sedoric format (reverse-engineered & validated, see bbsgo CLIENT_ORIC):
 *   - MFM container: per track, sectors are framed by A1 A1 A1 FE (ID:
 *     track,side,sector) then A1 A1 A1 FB (256 data bytes + 2 CRC bytes).
 *   - VTOC at track 20 sector 2: +2,+3 free sectors (LE), +4,+5 file count (LE).
 *   - Directory first sector at track 20 sector 4: +0,+1 = link to next dir
 *     sector (00 00 = end), +2 = high-water mark (offset of first free entry),
 *     entries of 16 bytes from offset 16: name[9] ext[3] track sector nsec status.
 *     Sedoric V4 valid-file status = 0x40 (bit7 clear; 0xC0 = deleted/V3).
 *   - File descriptor sector (pointed by the directory entry): +2 = 0xFF,
 *     +4,+5 load address (LE), +6,+7 end address (LE), +9,+10 data sector count
 *     (BE), +12.. sector map = (track,sector) pairs terminated by 00 00.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define SECSZ        256
#define TRK_RAW      6400          /* MFM track size */
#define MFM_HDR      256           /* MFM_DISK header */
#define DIR_TRACK    20
#define DIR_SECTOR   4
#define VTOC_SECTOR  2
#define MAX_SIDES    2
#define MAX_TRACKS   82
#define MAX_SECT     18            /* sectors are numbered 1..17 */

/* offset (in the raw .dsk buffer) of each sector's 256 data bytes, or 0 if absent */
static long sec_off[MAX_SIDES][MAX_TRACKS][MAX_SECT];
static uint8_t *disk;              /* whole raw .dsk image */
static long disk_size;
static int num_tracks;            /* physical tracks per side */

/* CRC-16/CCITT (poly 0x1021, init 0xFFFF) over the 4 mark bytes + 256 data */
static uint16_t crc16(const uint8_t *p, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

/* logical track -> physical (side, track) : Sedoric numbers side 1 as track>=80 */
static void l2p(int track, int *side, int *ptrack) {
    if (track >= num_tracks) { *side = 1; *ptrack = track - num_tracks; }
    else                     { *side = 0; *ptrack = track; }
}

static long goff(int track, int sector) {
    int s, pt; l2p(track, &s, &pt);
    if (s >= MAX_SIDES || pt >= MAX_TRACKS || sector >= MAX_SECT) return 0;
    return sec_off[s][pt][sector];
}

/* read/write a logical sector's 256 data bytes (in place, MFM data field) */
static int read_sec(int track, int sector, uint8_t *buf) {
    long o = goff(track, sector);
    if (!o) return 0;
    memcpy(buf, disk + o, SECSZ);
    return 1;
}
static int write_sec(int track, int sector, const uint8_t *buf) {
    long o = goff(track, sector);
    if (!o) return 0;
    memcpy(disk + o, buf, SECSZ);
    /* refresh the data-field CRC (the 4 mark bytes A1 A1 A1 FB precede the data) */
    uint8_t tmp[4 + SECSZ];
    tmp[0] = tmp[1] = tmp[2] = 0xA1; tmp[3] = 0xFB;
    memcpy(tmp + 4, buf, SECSZ);
    uint16_t c = crc16(tmp, 4 + SECSZ);
    disk[o + SECSZ]     = (uint8_t)(c >> 8);
    disk[o + SECSZ + 1] = (uint8_t)(c & 0xFF);
    return 1;
}

/* scan the MFM container and record every sector's data-field offset */
static void build_index(void) {
    uint32_t sides  = disk[8]  | (uint32_t)disk[9]  << 8;
    uint32_t tracks = disk[12] | (uint32_t)disk[13] << 8;
    if (sides  > MAX_SIDES)  sides  = MAX_SIDES;
    if (tracks > MAX_TRACKS) tracks = MAX_TRACKS;
    num_tracks = (int)tracks;
    memset(sec_off, 0, sizeof(sec_off));
    for (uint32_t s = 0; s < sides; s++) {
        for (uint32_t t = 0; t < tracks; t++) {
            long base = MFM_HDR + (long)(s * tracks + t) * TRK_RAW;
            if (base + TRK_RAW > disk_size) break;
            const uint8_t *td = disk + base;
            for (int i = 0; i < TRK_RAW - 4; i++) {
                if (td[i]==0xA1 && td[i+1]==0xA1 && td[i+2]==0xA1 && td[i+3]==0xFE) {
                    int isec = td[i+6];
                    for (int j = i + 10; j < i + 60 && j < TRK_RAW - 260; j++) {
                        if (td[j]==0xA1 && td[j+1]==0xA1 && td[j+2]==0xA1 && td[j+3]==0xFB) {
                            if (isec >= 1 && isec < MAX_SECT)
                                sec_off[s][t][isec] = base + j + 4;
                            break;
                        }
                    }
                }
            }
        }
    }
}

/* ----- .tap parsing ----- */
typedef struct {
    char     name[16];
    uint8_t  type, autorun;
    uint16_t start, end;
    const uint8_t *data;
    long     datalen;
} tap_t;

static int parse_tap(const uint8_t *buf, long len, tap_t *out) {
    /* find the 0x24 header marker after the leading 0x16 sync bytes */
    long i = 0;
    while (i < len && buf[i] == 0x16) i++;
    if (i >= len || buf[i] != 0x24) { fprintf(stderr, "tap: no 0x24 marker\n"); return 0; }
    while (i < len && buf[i] == 0x24) i++;          /* skip marker byte(s) */
    if (i + 9 > len) return 0;
    /* header: 00 00 type autorun end(BE) start(BE) 00 name\0 */
    out->type    = buf[i+2];
    out->autorun = buf[i+3];
    out->end     = (uint16_t)(buf[i+4] << 8 | buf[i+5]);
    out->start   = (uint16_t)(buf[i+6] << 8 | buf[i+7]);
    i += 9;
    int k = 0;
    while (i < len && buf[i] != 0 && k < 15) out->name[k++] = (char)buf[i++];
    out->name[k] = 0;
    while (i < len && buf[i] != 0) i++;             /* to the name terminator */
    if (i < len) i++;                               /* skip the 0x00 */
    out->data = buf + i;
    long avail = len - i;
    long want  = (long)out->end - (long)out->start + 1;
    out->datalen = (want > 0 && want <= avail) ? want : avail;
    return 1;
}

static uint8_t *slurp(const char *fn, long *sz) {
    FILE *f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", fn); return NULL; }
    fseek(f, 0, SEEK_END); *sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)*sz);
    if (!p || fread(p, 1, (size_t)*sz, f) != (size_t)*sz) { fclose(f); free(p); return NULL; }
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *base = NULL, *named = NULL;
    for (int a = 1; a < argc; a++) {
        if      (!strcmp(argv[a], "-o") && a+1 < argc) out   = argv[++a];
        else if (!strcmp(argv[a], "-b") && a+1 < argc) base  = argv[++a];
        else if (!strcmp(argv[a], "-n") && a+1 < argc) named = argv[++a];
        else if (argv[a][0] != '-') in = argv[a];
    }
    if (!in || !out || !base) {
        fprintf(stderr, "Usage: %s <input.tap> -o <output.dsk> -b <base.dsk> [-n NAME.EXT]\n", argv[0]);
        return 1;
    }

    long tlen; uint8_t *tbuf = slurp(in, &tlen);
    if (!tbuf) return 1;
    tap_t tap; memset(&tap, 0, sizeof tap);
    if (!parse_tap(tbuf, tlen, &tap)) return 1;

    disk = slurp(base, &disk_size);
    if (!disk) return 1;
    if (disk_size <= MFM_HDR || memcmp(disk, "MFM_DISK", 8) != 0) {
        fprintf(stderr, "base %s is not an MFM_DISK image\n", base); return 1;
    }
    build_index();

    /* Sedoric name (9) + ext (3): from -n, else from the tap name */
    char name[10], ext[4];
    memset(name, ' ', 9); name[9] = 0;
    memset(ext, ' ', 3);  ext[3] = 0;
    if (named) {
        const char *dot = strchr(named, '.');
        int nl = dot ? (int)(dot - named) : (int)strlen(named);
        if (nl > 9) nl = 9;
        for (int i = 0; i < nl; i++) name[i] = (char)toupper((unsigned char)named[i]);
        if (dot) { int el = (int)strlen(dot+1); if (el > 3) el = 3;
                   for (int i = 0; i < el; i++) ext[i] = (char)toupper((unsigned char)dot[1+i]); }
    } else {
        int nl = (int)strlen(tap.name); if (nl > 9) nl = 9;
        for (int i = 0; i < nl; i++) name[i] = (char)toupper((unsigned char)tap.name[i]);
        memcpy(ext, "COM", 3);
    }

    int ndata = (int)((tap.datalen + SECSZ - 1) / SECSZ);
    if (ndata < 1) ndata = 1;
    int total = ndata + 1;                          /* descriptor + data sectors */

    /* allocate free sectors from a safe region (track 21..) present in the image */
    int alloc_t[600], alloc_s[600], na = 0;
    int t = 21, s = 1;
    while (na < total && t < num_tracks) {
        if (t != DIR_TRACK && goff(t, s)) { alloc_t[na] = t; alloc_s[na] = s; na++; }
        if (++s > 17) { s = 1; t++; }
    }
    if (na < total) { fprintf(stderr, "not enough free sectors\n"); return 1; }
    int desc_t = alloc_t[0], desc_s = alloc_s[0];

    /* descriptor sector: load/end address + sector map */
    uint8_t d[SECSZ]; memset(d, 0, SECSZ);
    d[2] = 0xFF;
    d[4] = tap.start & 0xFF; d[5] = tap.start >> 8;
    d[6] = tap.end   & 0xFF; d[7] = tap.end   >> 8;
    d[9] = (uint8_t)(ndata >> 8); d[10] = (uint8_t)(ndata & 0xFF);
    int p = 12;
    for (int i = 1; i < total; i++) { d[p++] = (uint8_t)alloc_t[i]; d[p++] = (uint8_t)alloc_s[i]; }
    d[p] = 0; d[p+1] = 0;
    write_sec(desc_t, desc_s, d);

    /* data sectors */
    for (int i = 1; i < total; i++) {
        uint8_t b[SECSZ]; memset(b, 0, SECSZ);
        long off = (long)(i - 1) * SECSZ;
        long n = tap.datalen - off; if (n > SECSZ) n = SECSZ; if (n < 0) n = 0;
        memcpy(b, tap.data + off, (size_t)n);
        write_sec(alloc_t[i], alloc_s[i], b);
    }

    /* directory entry in track 20 sector 4 */
    uint8_t dir[SECSZ];
    if (!read_sec(DIR_TRACK, DIR_SECTOR, dir)) { fprintf(stderr, "no dir sector\n"); return 1; }
    int nent = 0;
    for (int e = 16; e < SECSZ; e += 16) if (dir[e] != 0 || dir[e+15] != 0) nent++;
    int slot = 16 + nent * 16;
    if (slot + 16 > SECSZ) { fprintf(stderr, "directory sector full (extend not implemented)\n"); return 1; }
    memcpy(dir + slot, name, 9);
    memcpy(dir + slot + 9, ext, 3);
    dir[slot + 12] = (uint8_t)desc_t;
    dir[slot + 13] = (uint8_t)desc_s;
    dir[slot + 14] = (uint8_t)total;
    dir[slot + 15] = 0x40;                          /* Sedoric V4 normal file */
    dir[2] = (uint8_t)(slot + 16);                  /* high-water mark */
    write_sec(DIR_TRACK, DIR_SECTOR, dir);

    /* VTOC: free -= total, files += 1 */
    uint8_t v[SECSZ];
    if (!read_sec(DIR_TRACK, VTOC_SECTOR, v)) { fprintf(stderr, "no VTOC\n"); return 1; }
    int freecnt = (v[2] | v[3] << 8) - total;
    int filecnt = (v[4] | v[5] << 8) + 1;
    v[2] = freecnt & 0xFF; v[3] = (freecnt >> 8) & 0xFF;
    v[4] = filecnt & 0xFF; v[5] = (filecnt >> 8) & 0xFF;
    write_sec(DIR_TRACK, VTOC_SECTOR, v);

    FILE *fo = fopen(out, "wb");
    if (!fo || fwrite(disk, 1, (size_t)disk_size, fo) != (size_t)disk_size) {
        fprintf(stderr, "cannot write %s\n", out); return 1;
    }
    fclose(fo);

    printf("Injected %.9s.%.3s  (%d sectors, load $%04X-$%04X) into %s\n",
           name, ext, total, tap.start, tap.end, out);
    printf("  base=%s  free=%d  files=%d\n", base, freecnt, filecnt);
    return 0;
}
