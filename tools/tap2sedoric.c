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
 *                 [-a] [-e EXEC_HEX] [-i "INIST"]
 *       -a          mark the file AUTO (auto-executable ML) — set by default
 *                   when the .tap header carries a non-zero autorun byte.
 *       -e EXEC_HEX execution address for an AUTO file (default = load address).
 *       -i "CMD"    write CMD as the boot INIST (autoexec) so the disk runs it
 *                   at boot (e.g. -i "MYPROG" or -i 'CLOAD"...').
 *
 * - <base.dsk> must be a Sedoric MFM_DISK image (e.g. a blank formatted V4 disk).
 * - The downloaded file (from the Oric terminal's CSAVE .tap) is written into
 *   free sectors as a Sedoric file: a descriptor sector (load/end address +
 *   sector map) followed by the data sectors, plus a directory entry and the
 *   VTOC counters updated. Verified by booting Sedoric and running DIR.
 * - Repeated injections into the same disk no longer collide: the allocator
 *   marks the sectors already consumed by existing files (directory walk +
 *   descriptor sector maps) as used before allocating fresh sectors.
 *
 * On-disk Sedoric format. Fields verified byte-exact against A. Chéramy,
 * "SEDORIC 3.0 à NU" (cross-checked with the SCUMM-Oric tooling, which produces
 * real auto-booting disks) — see docs/SEDORIC.md:
 *   - MFM container: per track, sectors are framed by A1 A1 A1 FE (ID:
 *     track,side,sector) then A1 A1 A1 FB (256 data bytes + 2 CRC bytes).
 *   - VTOC at track 20 sector 2: +2,+3 free sectors (LE), +4,+5 file count (LE).
 *   - System sector at track 20 sector 1: +0x1E..+0x59 INIST (60 bytes, ASCII
 *     boot commands separated by ':', terminated by 00) — the boot autoexec.
 *   - Directory first sector at track 20 sector 4: +0,+1 = link to next dir
 *     sector (00 00 = end), +2 = high-water mark (offset of first free entry),
 *     entries of 16 bytes from offset 16: name[9] ext[3] track sector nsec status.
 *     Sedoric V4 valid-file status = 0x40 (bit7 clear; 0xC0 = deleted/V3).
 *   - File descriptor sector (pointed by the directory entry):
 *       +0,+1 link to next descriptor (00 00 = none) | +2 = 0xFF | +3 = type
 *       (b0=AUTO, b6=data block, b7=BASIC) → 0x40 = ML data, 0x41 = AUTO ML |
 *       +4,+5 load (LE) | +6,+7 end (LE) | +8,+9 exec address if AUTO (LE) |
 *       +0xA,+0xB data sector count (LE) | +0xC.. sector map = (track,sector)
 *       pairs terminated by 00 00.
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
/* logical (track,sector) already consumed by an existing file — avoids the
 * multi-file collision where a second injection reallocates from track 21 sec 1
 * and overwrites the first file's descriptor (see docs/SEDORIC.md). */
static uint8_t used_map[2 * MAX_TRACKS][MAX_SECT];
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

/* mark (track,sector) used in used_map (logical track index) */
static void mark_used(int track, int sector) {
    if (track >= 0 && track < 2 * MAX_TRACKS && sector >= 1 && sector < MAX_SECT)
        used_map[track][sector] = 1;
}

/* Walk the directory and every file's descriptor sector map, marking each
 * consumed sector (and every descriptor sector) as used. Called before
 * allocation so repeated injections into the same disk never collide. */
static void scan_used(void) {
    memset(used_map, 0, sizeof(used_map));
    int dt = DIR_TRACK, ds = DIR_SECTOR, guard = 0;
    while (guard++ < 64) {                       /* follow the directory chain */
        uint8_t dir[SECSZ];
        if (!read_sec(dt, ds, dir)) break;
        mark_used(dt, ds);                       /* the catalogue sector itself */
        for (int e = 16; e < SECSZ; e += 16) {
            if (dir[e] == 0 && dir[e + 15] == 0) continue;   /* empty slot */
            if (dir[e + 15] & 0x80) continue;                /* deleted (bit7) */
            int fdt = dir[e + 12], fds = dir[e + 13];        /* descriptor loc */
            int cdt = fdt, cds = fds, dg = 0, first = 1;
            while (dg++ < 64) {                  /* follow the descriptor chain */
                uint8_t desc[SECSZ];
                if (!read_sec(cdt, cds, desc)) break;
                mark_used(cdt, cds);             /* the descriptor sector itself */
                /* map starts at +0x0C in the first descriptor, +0x02 in a
                 * continuation (which only has the 2-byte link header) */
                for (int p = first ? 12 : 2; p + 1 < SECSZ; p += 2) {
                    if (desc[p] == 0 && desc[p + 1] == 0) break;
                    mark_used(desc[p], desc[p + 1]);
                }
                if (desc[0] == 0 && desc[1] == 0) break;      /* no next descr. */
                cdt = desc[0]; cds = desc[1]; first = 0;
            }
        }
        if (dir[0] == 0 && dir[1] == 0) break;               /* no next dir sec. */
        dt = dir[0]; ds = dir[1];
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
    const char *in = NULL, *out = NULL, *base = NULL, *named = NULL, *inist = NULL;
    int autorun = 0;                 /* -1 off, +1 on, 0 = from .tap header */
    long exec_arg = -1;              /* -e EXEC_HEX, -1 = default (load addr) */
    for (int a = 1; a < argc; a++) {
        if      (!strcmp(argv[a], "-o") && a+1 < argc) out   = argv[++a];
        else if (!strcmp(argv[a], "-b") && a+1 < argc) base  = argv[++a];
        else if (!strcmp(argv[a], "-n") && a+1 < argc) named = argv[++a];
        else if (!strcmp(argv[a], "-i") && a+1 < argc) inist = argv[++a];
        else if (!strcmp(argv[a], "-e") && a+1 < argc) exec_arg = strtol(argv[++a], NULL, 16);
        else if (!strcmp(argv[a], "-a")) autorun = 1;
        else if (argv[a][0] != '-') in = argv[a];
    }
    if (!in || !out || !base) {
        fprintf(stderr, "Usage: %s <input.tap> -o <output.dsk> -b <base.dsk> "
                        "[-n NAME.EXT] [-a] [-e EXEC_HEX] [-i \"INIST\"]\n", argv[0]);
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

    /* Chained descriptors (validated in-situ, see docs/SEDORIC.md): the first
     * descriptor carries the 12-byte header then the (track,sector) map from
     * +0x0C (122 pairs max); each further descriptor is a 2-byte link (+0,+1)
     * then its map from +0x02 (127 pairs max). +0,+1 links to the next. */
    const int FIRST_CAP = (SECSZ - 12) / 2;         /* 122 pairs */
    const int CONT_CAP  = (SECSZ - 2) / 2;          /* 127 pairs */
    int ndesc = (ndata <= FIRST_CAP)
              ? 1 : 1 + (ndata - FIRST_CAP + CONT_CAP - 1) / CONT_CAP;
    int total = ndesc + ndata;                      /* descriptors + data sectors */
    if (total > 255) { fprintf(stderr, "file too large: %d sectors > 255\n", total); return 1; }

    /* allocate free sectors from a safe region (track 21..) present in the image,
     * skipping sectors already consumed by existing files (multi-file safe) */
    scan_used();
    int alloc_t[600], alloc_s[600], na = 0;
    int t = 21, s = 1;
    while (na < total && t < num_tracks) {
        if (t != DIR_TRACK && goff(t, s) && !used_map[t][s]) {
            alloc_t[na] = t; alloc_s[na] = s; na++;
        }
        if (++s > 17) { s = 1; t++; }
    }
    if (na < total) { fprintf(stderr, "not enough free sectors\n"); return 1; }
    int desc_t = alloc_t[0], desc_s = alloc_s[0];    /* first descriptor sector */
    int ddata = ndesc;                               /* first data sector index in alloc[] */

    /* AUTO flag: -a forces it, else honour the .tap autorun byte */
    int is_auto = (autorun == 1) || (autorun == 0 && tap.autorun != 0);
    uint16_t execaddr = (exec_arg >= 0) ? (uint16_t)exec_arg : tap.start;

    /* descriptor sectors (verified byte-exact vs SEDORIC 3.0 manual, see file header):
     * +2 FF | +3 type | +4,5 load LE | +6,7 end LE | +8,9 exec LE | +A,B nsec LE */
    int idx = 0;                                     /* index into data sectors */
    for (int di = 0; di < ndesc; di++) {
        uint8_t d[SECSZ]; memset(d, 0, SECSZ);
        int p, cap;
        if (di == 0) {
            d[2] = 0xFF;
            d[3] = is_auto ? 0x41 : 0x40;            /* b6=data block, b0=AUTO */
            d[4] = tap.start & 0xFF; d[5] = tap.start >> 8;
            d[6] = tap.end   & 0xFF; d[7] = tap.end   >> 8;
            if (is_auto) { d[8] = execaddr & 0xFF; d[9] = execaddr >> 8; }
            d[10] = (uint8_t)(ndata & 0xFF); d[11] = (uint8_t)(ndata >> 8);
            p = 12; cap = FIRST_CAP;
        } else {
            p = 2; cap = CONT_CAP;                   /* continuation: link then map from +0x02 */
        }
        int n = ndata - idx; if (n > cap) n = cap;
        for (int i = 0; i < n; i++) {
            d[p++] = (uint8_t)alloc_t[ddata + idx + i];
            d[p++] = (uint8_t)alloc_s[ddata + idx + i];
        }
        idx += n;
        if (di < ndesc - 1) {                        /* link -> next descriptor */
            d[0] = (uint8_t)alloc_t[di + 1]; d[1] = (uint8_t)alloc_s[di + 1];
        } else if (p + 1 < SECSZ) {                  /* terminator on the last */
            d[p] = 0; d[p + 1] = 0;
        }
        write_sec(alloc_t[di], alloc_s[di], d);
    }

    /* data sectors */
    for (int i = 0; i < ndata; i++) {
        uint8_t b[SECSZ]; memset(b, 0, SECSZ);
        long off = (long)i * SECSZ;
        long n = tap.datalen - off; if (n > SECSZ) n = SECSZ; if (n < 0) n = 0;
        memcpy(b, tap.data + off, (size_t)n);
        write_sec(alloc_t[ddata + i], alloc_s[ddata + i], b);
    }

    /* directory entry: walk the catalogue chain (t20 s4 -> +0,+1 link -> ...)
     * to find a sector with a free slot; if the whole chain is full, allocate
     * a fresh sector and link it. Catalogue sectors are located by (track,
     * sector) links, not a fixed area (SEDORIC 3.0 manual, ANNEXE 7) — a new
     * catalogue sector may be any free sector the previous one links to. */
    uint8_t dir[SECSZ];
    int cat_t = DIR_TRACK, cat_s = DIR_SECTOR, slot = -1, cat_guard = 0;
    int new_cat = 0;
    for (;;) {
        if (!read_sec(cat_t, cat_s, dir)) { fprintf(stderr, "no dir sector\n"); return 1; }
        int nent = 0;
        for (int e = 16; e < SECSZ; e += 16) if (dir[e] != 0 || dir[e+15] != 0) nent++;
        int s = 16 + nent * 16;
        if (s + 16 <= SECSZ) { slot = s; break; }      /* free slot in this sector */
        if (dir[0] == 0 && dir[1] == 0) break;         /* end of chain, all full */
        cat_t = dir[0]; cat_s = dir[1];
        if (++cat_guard > 64) { fprintf(stderr, "directory chain too long\n"); return 1; }
    }
    if (slot < 0) {
        /* chain full: allocate one more free sector as a new catalogue sector */
        for (int i = 0; i < total; i++) used_map[alloc_t[i]][alloc_s[i]] = 1;
        int nt = 21, ns = 1, ncat_t = 0, ncat_s = 0, found = 0;
        while (nt < num_tracks) {
            if (nt != DIR_TRACK && goff(nt, ns) && !used_map[nt][ns]) {
                ncat_t = nt; ncat_s = ns; found = 1; break;
            }
            if (++ns > 17) { ns = 1; nt++; }
        }
        if (!found) { fprintf(stderr, "no free sector for new directory\n"); return 1; }
        dir[0] = (uint8_t)ncat_t; dir[1] = (uint8_t)ncat_s;   /* link previous -> new */
        write_sec(cat_t, cat_s, dir);
        memset(dir, 0, SECSZ);                                /* fresh empty catalogue */
        cat_t = ncat_t; cat_s = ncat_s; slot = 16; new_cat = 1;
    }
    memcpy(dir + slot, name, 9);
    memcpy(dir + slot + 9, ext, 3);
    dir[slot + 12] = (uint8_t)desc_t;
    dir[slot + 13] = (uint8_t)desc_s;
    dir[slot + 14] = (uint8_t)total;
    dir[slot + 15] = 0x40;                          /* Sedoric V4 normal file */
    dir[2] = (uint8_t)(slot + 16);                  /* high-water mark */
    write_sec(cat_t, cat_s, dir);

    /* VTOC: free -= total (+1 if a new catalogue sector was consumed), files += 1 */
    uint8_t v[SECSZ];
    if (!read_sec(DIR_TRACK, VTOC_SECTOR, v)) { fprintf(stderr, "no VTOC\n"); return 1; }
    int freecnt = (v[2] | v[3] << 8) - total - new_cat;
    int filecnt = (v[4] | v[5] << 8) + 1;
    v[2] = freecnt & 0xFF; v[3] = (freecnt >> 8) & 0xFF;
    v[4] = filecnt & 0xFF; v[5] = (filecnt >> 8) & 0xFF;
    write_sec(DIR_TRACK, VTOC_SECTOR, v);

    /* INIST (boot autoexec): system sector = track 20 sector 1, offset 0x1E,
     * 60 bytes of ASCII commands terminated by 00 (SEDORIC 3.0 manual) */
    if (inist) {
        size_t il = strlen(inist);
        if (il >= 60) { fprintf(stderr, "INIST too long (%zu >= 60)\n", il); return 1; }
        uint8_t sy[SECSZ];
        if (!read_sec(DIR_TRACK, 1, sy)) { fprintf(stderr, "no system sector\n"); return 1; }
        memset(sy + 0x1E, 0, 60);
        memcpy(sy + 0x1E, inist, il);
        sy[0x1E + il] = 0x00;
        write_sec(DIR_TRACK, 1, sy);
    }

    FILE *fo = fopen(out, "wb");
    if (!fo || fwrite(disk, 1, (size_t)disk_size, fo) != (size_t)disk_size) {
        fprintf(stderr, "cannot write %s\n", out); return 1;
    }
    fclose(fo);

    printf("Injected %.9s.%.3s  (%d sectors, load $%04X-$%04X%s) into %s\n",
           name, ext, total, tap.start, tap.end,
           is_auto ? "" : ", not AUTO", out);
    if (is_auto) printf("  AUTO exec $%04X\n", execaddr);
    if (inist)   printf("  INIST=\"%s\"\n", inist);
    printf("  base=%s  free=%d  files=%d\n", base, freecnt, filecnt);
    return 0;
}
