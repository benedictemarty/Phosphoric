/**
 * @file sedoric_info.c
 * @brief Sedoric disk inspector -- dumps the VTOC counters, the directory and,
 *        for each file, the decoded descriptor sector (type / load / end / exec
 *        / data-sector count / sector map). Port and generalisation of the
 *        SCUMM-Oric analyze_sedoric_vtoc.py, extended with descriptor decoding.
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-07-15
 * @version 1.0.0
 *
 * Self-contained (no Phosphoric libs): build with
 *     cc -O2 -o sedoric-info sedoric_info.c
 *
 * Usage:
 *     sedoric-info <disk.dsk> [--check FREE:FILES]
 *
 * - <disk.dsk> is a Sedoric MFM_DISK image, or a RAW side-major image
 *   (all sectors of side 0 then side 1, 256 bytes each; header != "MFM_DISK").
 * - --check FREE:FILES makes it a regression guard: exits non-zero if the VTOC
 *   free-sector / file counters differ from the expected values (like the SCUMM
 *   analyze_sedoric_vtoc.py guard that locks in the understood format).
 *
 * On-disk format verified byte-exact against A. Chéramy "SEDORIC 3.0"
 * (sedna3_0.pdf) -- see docs/SEDORIC.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define SECSZ        256
#define TRK_RAW      6400
#define MFM_HDR      256
#define DIR_TRACK    20
#define DIR_SECTOR   4
#define VTOC_SECTOR  2
#define SYS_SECTOR   1
#define MAX_SIDES    2
#define MAX_TRACKS   82
#define MAX_SECT     18

static long sec_off[MAX_SIDES][MAX_TRACKS][MAX_SECT];
static uint8_t *disk;
static long disk_size;
static int num_tracks;
static int raw_mode;                 /* 1 = raw side-major, 0 = MFM container */
static int raw_sectors = 17;

static void l2p(int track, int *side, int *ptrack) {
    if (track >= num_tracks) { *side = 1; *ptrack = track - num_tracks; }
    else                     { *side = 0; *ptrack = track; }
}

static long goff(int track, int sector) {
    int s, pt; l2p(track, &s, &pt);
    if (s >= MAX_SIDES || pt >= MAX_TRACKS || sector >= MAX_SECT) return 0;
    if (raw_mode) {
        if (sector < 1 || sector > raw_sectors) return 0;
        return ((long)(s * num_tracks + pt) * raw_sectors + (sector - 1)) * SECSZ;
    }
    return sec_off[s][pt][sector];
}

static int read_sec(int track, int sector, uint8_t *buf) {
    long o = goff(track, sector);
    if (!o || o + SECSZ > disk_size) return 0;
    memcpy(buf, disk + o, SECSZ);
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

static uint8_t *slurp(const char *fn, long *sz) {
    FILE *f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", fn); return NULL; }
    fseek(f, 0, SEEK_END); *sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)*sz);
    if (!p || fread(p, 1, (size_t)*sz, f) != (size_t)*sz) { fclose(f); free(p); return NULL; }
    fclose(f);
    return p;
}

/* decode and print one file's descriptor chain; return data sectors counted */
static int dump_descriptor(int dt, int ds) {
    int cdt = dt, cds = ds, guard = 0, mapped = 0;
    while (guard++ < 64) {
        uint8_t d[SECSZ];
        if (!read_sec(cdt, cds, d)) { printf("      <descriptor t%d s%d unreadable>\n", cdt, cds); break; }
        if (cdt == dt && cds == ds) {
            uint16_t load = d[4] | d[5] << 8;
            uint16_t end  = d[6] | d[7] << 8;
            uint16_t exec = d[8] | d[9] << 8;
            uint16_t nsec = d[10] | d[11] << 8;
            printf("      type=$%02X%s  load=$%04X end=$%04X exec=$%04X nsec=%u\n",
                   d[3], (d[3] & 0x01) ? " AUTO" : "", load, end, exec, nsec);
        }
        for (int p = 12; p + 1 < SECSZ; p += 2) {
            if (d[p] == 0 && d[p + 1] == 0) break;
            mapped++;
        }
        if (d[0] == 0 && d[1] == 0) break;
        cdt = d[0]; cds = d[1];
    }
    return mapped;
}

int main(int argc, char **argv) {
    const char *in = NULL, *check = NULL;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--check") && a + 1 < argc) check = argv[++a];
        else if (argv[a][0] != '-') in = argv[a];
    }
    if (!in) { fprintf(stderr, "Usage: %s <disk.dsk> [--check FREE:FILES]\n", argv[0]); return 1; }

    disk = slurp(in, &disk_size);
    if (!disk) return 1;
    if (disk_size > 8 && memcmp(disk, "MFM_DISK", 8) == 0) {
        raw_mode = 0;
        build_index();
        printf("Image: %s (MFM_DISK, %d tracks/side)\n", in, num_tracks);
    } else {
        /* raw side-major: assume 2 sides, 17 sectors; derive tracks from size */
        raw_mode = 1;
        num_tracks = (int)(disk_size / (2L * raw_sectors * SECSZ));
        if (num_tracks < 1) num_tracks = (int)(disk_size / (raw_sectors * SECSZ));
        printf("Image: %s (RAW side-major, %d tracks/side, %d sectors)\n",
               in, num_tracks, raw_sectors);
    }

    uint8_t v[SECSZ];
    int freecnt = -1, filecnt = -1;
    if (read_sec(DIR_TRACK, VTOC_SECTOR, v)) {
        freecnt = v[2] | v[3] << 8;
        filecnt = v[4] | v[5] << 8;
        printf("VTOC (t20 s2): free=%d  files=%d\n", freecnt, filecnt);
    } else {
        printf("VTOC: <unreadable>\n");
    }

    uint8_t sy[SECSZ];
    if (read_sec(DIR_TRACK, SYS_SECTOR, sy)) {
        char nm[22]; memcpy(nm, sy + 9, 21); nm[21] = 0;
        for (int i = 0; i < 21; i++) if (!isprint((unsigned char)nm[i])) nm[i] = ' ';
        printf("Disk name: \"%s\"\n", nm);
        if (sy[0x1E] && isprint(sy[0x1E])) {
            char inist[61]; int k = 0;
            for (int i = 0x1E; i < 0x1E + 60 && sy[i]; i++) inist[k++] = (char)sy[i];
            inist[k] = 0;
            printf("INIST (autoexec): \"%s\"\n", inist);
        }
    }

    printf("Directory:\n");
    int dt = DIR_TRACK, ds = DIR_SECTOR, guard = 0, nfiles = 0;
    while (guard++ < 64) {
        uint8_t dir[SECSZ];
        if (!read_sec(dt, ds, dir)) break;
        for (int e = 16; e < SECSZ; e += 16) {
            if (dir[e] == 0 && dir[e + 15] == 0) continue;
            char name[10], ext[4];
            memcpy(name, dir + e, 9); name[9] = 0;
            memcpy(ext, dir + e + 9, 3); ext[3] = 0;
            int fdt = dir[e + 12], fds = dir[e + 13], nsec = dir[e + 14];
            int deleted = (dir[e + 15] & 0x80) != 0;
            printf("  %-9.9s.%-3.3s  desc=t%d s%d  nsec=%d  status=$%02X%s\n",
                   name, ext, fdt, fds, nsec, dir[e + 15], deleted ? " (deleted)" : "");
            if (!deleted) { dump_descriptor(fdt, fds); nfiles++; }
        }
        if (dir[0] == 0 && dir[1] == 0) break;
        dt = dir[0]; ds = dir[1];
    }
    printf("Total live files: %d\n", nfiles);

    if (check) {
        int wf = -1, wfi = -1;
        if (sscanf(check, "%d:%d", &wf, &wfi) != 2) {
            fprintf(stderr, "--check expects FREE:FILES\n"); return 2;
        }
        if (wf != freecnt || wfi != filecnt) {
            fprintf(stderr, "CHECK FAILED: expected free=%d files=%d, got free=%d files=%d\n",
                    wf, wfi, freecnt, filecnt);
            return 3;
        }
        printf("CHECK OK: free=%d files=%d\n", freecnt, filecnt);
    }
    return 0;
}
