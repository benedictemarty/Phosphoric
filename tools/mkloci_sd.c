/* SPDX-License-Identifier: EUPL-1.2 */
/* tools/mkloci_sd.c — Sprint 34ao polish
 *
 * Builds a FAT16 superfloppy SD image populated with the host files
 * given on the command line. The image is consumable by `--loci-sdimg`.
 *
 * Usage:
 *   mkloci_sd <output.img> <size_mb> [<host_file> ...]
 *
 *   - <size_mb> is the image size in megabytes (4..2047, FAT16 cap).
 *   - host files are copied to the root; their basenames are normalized
 *     to 8.3 (uppercase, illegal chars rejected, names truncated).
 *
 * Limitations:
 *   - Root only (no subdir creation).
 *   - 8.3 names only.
 *   - No FAT32 emit (use FAT16 for everything ≥ 4 MB and ≤ ~2 GB).
 *
 * This tool is the write-side counterpart of src/io/loci_sdimg.c which
 * is read-only. Eventual sprint 34ap will fold write support into the
 * runtime backend.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BPS  512u
#define SPC  4u            /* sectors per cluster (2 KB clusters) */
#define RSV  1u
#define NF   2u
#define RE   64u           /* root entries */

static void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* Extract basename without directory path. */
static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Normalize a host basename to a 11-char "NNNNNNNNEEE" FAT 8.3 form
 * (spaces pad, dot removed). Returns false if name is empty or
 * contains only invalid chars. */
static int normalize_83(const char* in, char out[12]) {
    memset(out, ' ', 11);
    out[11] = 0;
    const char* dot = strrchr(in, '.');
    size_t name_len = dot ? (size_t)(dot - in) : strlen(in);
    size_t ext_len  = dot ? strlen(dot + 1) : 0;
    if (name_len > 8) name_len = 8;
    if (ext_len  > 3) ext_len  = 3;
    if (name_len == 0) return 0;
    for (size_t i = 0; i < name_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '/' || c == '\\') return 0;
        out[i] = (char)toupper(c);
    }
    for (size_t i = 0; i < ext_len; i++) {
        out[8 + i] = (char)toupper((unsigned char)dot[1 + i]);
    }
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <output.img> <size_mb> [<host_file> ...]\n",
                argv[0]);
        return 1;
    }
    const char* out_path = argv[1];
    int size_mb = atoi(argv[2]);
    if (size_mb < 4 || size_mb > 2047) {
        fprintf(stderr, "ERROR: size_mb must be 4..2047 (FAT16)\n");
        return 1;
    }
    uint32_t total_sec = (uint32_t)size_mb * 1024u * 1024u / BPS;

    /* Pick a FAT size that covers all data clusters with margin. */
    uint32_t data_clusters_est = total_sec / SPC + 16;
    uint32_t fat_sz = (data_clusters_est * 2 + BPS - 1) / BPS;
    uint32_t root_sz = (RE * 32 + BPS - 1) / BPS;

    uint32_t fat_start = RSV;
    uint32_t root_start = fat_start + NF * fat_sz;
    uint32_t data_start = root_start + root_sz;

    if (data_start >= total_sec) {
        fprintf(stderr, "ERROR: image too small after reserving FAT + root\n");
        return 1;
    }
    uint32_t data_sec = total_sec - data_start;
    uint32_t count_of_clusters = data_sec / SPC;

    if (count_of_clusters < 4085) {
        fprintf(stderr, "ERROR: cluster count %u < 4085 (FAT12); use bigger size\n",
                count_of_clusters);
        return 1;
    }
    if (count_of_clusters >= 65525) {
        fprintf(stderr, "ERROR: cluster count %u >= 65525 (FAT32 territory);\n"
                        "FAT32 emit not supported by this tool — keep ≤ ~2 GB.\n",
                count_of_clusters);
        return 1;
    }

    printf("Image: %s\n", out_path);
    printf("  Total sectors: %u (%u MB)\n", total_sec, size_mb);
    printf("  FAT size: %u sectors  Root: %u sectors  Data start: sector %u\n",
           fat_sz, root_sz, data_start);
    printf("  Cluster count: %u (FAT16)\n", count_of_clusters);

    /* Allocate in-memory FAT and root. */
    uint8_t* fat = calloc(fat_sz, BPS);
    uint8_t* root = calloc(root_sz, BPS);
    if (!fat || !root) {
        fprintf(stderr, "ERROR: out of memory\n"); return 1;
    }
    put_u16(fat + 0, 0xFFF8);
    put_u16(fat + 2, 0xFFFF);

    /* Open output, write zeros for total_sec, then we'll rewrite key sectors. */
    FILE* fp = fopen(out_path, "wb+");
    if (!fp) { perror(out_path); return 1; }
    uint8_t zero_sec[BPS] = {0};
    for (uint32_t i = 0; i < total_sec; i++) fwrite(zero_sec, 1, BPS, fp);

    uint32_t next_free_cluster = 2;
    uint32_t entries_used = 0;

    for (int ai = 3; ai < argc; ai++) {
        const char* path = argv[ai];
        FILE* in = fopen(path, "rb");
        if (!in) { fprintf(stderr, "  WARN: skip %s (%s)\n", path, strerror(errno)); continue; }
        fseek(in, 0, SEEK_END);
        long fsz = ftell(in);
        fseek(in, 0, SEEK_SET);
        if (fsz < 0) { fclose(in); continue; }

        char name83[12];
        if (!normalize_83(basename_of(path), name83)) {
            fprintf(stderr, "  WARN: skip %s (bad basename)\n", path);
            fclose(in); continue;
        }
        if (entries_used >= RE - 1) {
            fprintf(stderr, "  WARN: skip %s (root dir full)\n", path);
            fclose(in); continue;
        }

        uint32_t clusters_needed = ((uint32_t)fsz + SPC * BPS - 1) / (SPC * BPS);
        if (clusters_needed == 0) clusters_needed = 1;
        if (next_free_cluster + clusters_needed > 2 + count_of_clusters) {
            fprintf(stderr, "  WARN: skip %s (data area full)\n", path);
            fclose(in); continue;
        }

        uint32_t first_cluster = next_free_cluster;
        /* Write FAT chain. */
        for (uint32_t i = 0; i < clusters_needed; i++) {
            uint32_t this_cl = next_free_cluster + i;
            uint16_t link = (i == clusters_needed - 1) ? 0xFFFF
                                                       : (uint16_t)(this_cl + 1);
            put_u16(fat + this_cl * 2, link);
        }

        /* Copy file data. */
        uint32_t lba = data_start + (first_cluster - 2) * SPC;
        fseek(fp, (long)lba * BPS, SEEK_SET);
        uint8_t buf[BPS];
        long remaining = fsz;
        while (remaining > 0) {
            size_t chunk = remaining > (long)BPS ? BPS : (size_t)remaining;
            memset(buf, 0, BPS);
            if (fread(buf, 1, chunk, in) != chunk) {
                fprintf(stderr, "  WARN: read error on %s\n", path); break;
            }
            fwrite(buf, 1, BPS, fp);
            remaining -= (long)chunk;
        }
        fclose(in);

        /* Dir entry. */
        uint8_t* e = root + entries_used * 32;
        memcpy(e, name83, 11);
        e[11] = 0x20;  /* archive */
        put_u16(e + 20, 0);
        put_u16(e + 26, (uint16_t)first_cluster);
        put_u32(e + 28, (uint32_t)fsz);
        entries_used++;

        printf("  + %-11.11s  %ld bytes  → cluster %u..%u\n",
               name83, fsz, first_cluster,
               first_cluster + clusters_needed - 1);

        next_free_cluster += clusters_needed;
    }

    /* Write BPB sector 0. */
    uint8_t bpb[BPS] = {0};
    bpb[0] = 0xEB; bpb[1] = 0x3C; bpb[2] = 0x90;
    memcpy(bpb + 3, "PHOSPHRC", 8);
    put_u16(bpb + 11, BPS);
    bpb[13] = SPC;
    put_u16(bpb + 14, RSV);
    bpb[16] = NF;
    put_u16(bpb + 17, RE);
    if (total_sec < 0x10000) put_u16(bpb + 19, (uint16_t)total_sec);
    else                     put_u32(bpb + 32, total_sec);
    bpb[21] = 0xF8;
    put_u16(bpb + 22, (uint16_t)fat_sz);
    bpb[510] = 0x55; bpb[511] = 0xAA;
    fseek(fp, 0, SEEK_SET);
    fwrite(bpb, 1, BPS, fp);

    /* Write both FAT copies. */
    for (uint32_t i = 0; i < NF; i++) {
        fseek(fp, (long)(fat_start + i * fat_sz) * BPS, SEEK_SET);
        fwrite(fat, 1, fat_sz * BPS, fp);
    }
    /* Write root dir. */
    fseek(fp, (long)root_start * BPS, SEEK_SET);
    fwrite(root, 1, root_sz * BPS, fp);

    fclose(fp);
    free(fat);
    free(root);
    printf("Done: %u file(s), %u clusters used.\n",
           entries_used, next_free_cluster - 2);
    return 0;
}
