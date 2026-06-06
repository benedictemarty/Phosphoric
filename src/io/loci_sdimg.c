/* LOCI SD raw image backend — FAT16/32 reader+writer. See header.
 *
 * Layout assumed (no MBR, "superfloppy" image):
 *   sector 0           = BPB (boot sector + parameter block)
 *   sector RsvdSecCnt  = first FAT
 *   sector RsvdSecCnt + NumFATs*FATSz = root dir (FAT16) or data (FAT32)
 *
 * All multi-byte fields in FAT structures are little-endian.
 */
#define _POSIX_C_SOURCE 200809L
#include "io/loci_sdimg.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SDIMG_MAX_FILES   16
#define SDIMG_MAX_DIRS     8
#define SDIMG_SECTOR_MAX 512
#define SDIMG_PATH_MAX   260

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

typedef enum {
    SDIMG_FS_FAT16,
    SDIMG_FS_FAT32
} sdimg_fs_t;

typedef struct {
    bool     used;
    bool     is_dir;
    bool     writable;            /* opened in W or R+W mode */
    uint32_t first_cluster;       /* 0 for FAT16 fixed root */
    uint32_t size_bytes;          /* 0 for directories */
    uint32_t cursor;              /* byte offset for files, entry index for dirs */
    uint32_t current_cluster;     /* cached cluster while walking */
    uint32_t cluster_byte_offset; /* current pos inside current_cluster */
    /* Dir entry location for size-update on write extension. */
    uint32_t dir_entry_lba;
    uint32_t dir_entry_off;
} sdimg_handle_t;

struct loci_sdimg_s {
    FILE*      fp;
    sdimg_fs_t fs_kind;
    uint32_t   total_size;
    bool       read_only;

    /* BPB-derived */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;        /* FAT16 only */
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;        /* FAT32 only */

    /* Derived */
    uint32_t fat_start_sector;
    uint32_t root_dir_start_sector; /* FAT16: first sector of root, FAT32: 0 */
    uint32_t root_dir_sectors;      /* FAT16 root size in sectors */
    uint32_t data_start_sector;     /* first sector of cluster 2 */
    uint32_t bytes_per_cluster;
    uint32_t count_of_clusters;

    sdimg_handle_t files[SDIMG_MAX_FILES];
    sdimg_handle_t dirs[SDIMG_MAX_DIRS];
};

/* ─── low-level I/O ──────────────────────────────────────────────── */

static bool read_sector(loci_sdimg_t* img, uint32_t lba, uint8_t* out) {
    if (fseek(img->fp, (long)lba * img->bytes_per_sector, SEEK_SET) != 0) return false;
    size_t n = fread(out, 1, img->bytes_per_sector, img->fp);
    return n == img->bytes_per_sector;
}

static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read the FAT entry for the given cluster. Returns next cluster, or
 * 0xFFFFFFFF on EOC, or 0 on error. */
static uint32_t read_fat_entry(loci_sdimg_t* img, uint32_t cluster) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    uint32_t offset = (img->fs_kind == SDIMG_FS_FAT32) ? cluster * 4u : cluster * 2u;
    uint32_t sec_idx = img->fat_start_sector + offset / img->bytes_per_sector;
    uint32_t sec_off = offset % img->bytes_per_sector;
    if (!read_sector(img, sec_idx, sec)) return 0;
    uint32_t next;
    if (img->fs_kind == SDIMG_FS_FAT32) {
        next = rd_u32(sec + sec_off) & 0x0FFFFFFFu;
        if (next >= 0x0FFFFFF8u) return 0xFFFFFFFFu;
    } else {
        next = rd_u16(sec + sec_off);
        if (next >= 0xFFF8u) return 0xFFFFFFFFu;
    }
    return next;
}

static uint32_t cluster_first_sector(const loci_sdimg_t* img, uint32_t cluster) {
    return img->data_start_sector + (cluster - 2) * img->sectors_per_cluster;
}

/* ─── BPB parsing ────────────────────────────────────────────────── */

static bool parse_bpb(loci_sdimg_t* img) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    if (fseek(img->fp, 0, SEEK_SET) != 0) return false;
    if (fread(sec, 1, SDIMG_SECTOR_MAX, img->fp) != SDIMG_SECTOR_MAX) return false;

    img->bytes_per_sector    = rd_u16(sec + 11);
    img->sectors_per_cluster = sec[13];
    img->reserved_sectors    = rd_u16(sec + 14);
    img->num_fats            = sec[16];
    img->root_entries        = rd_u16(sec + 17);
    uint16_t total16         = rd_u16(sec + 19);
    uint16_t fat_sz_16       = rd_u16(sec + 22);
    uint32_t total32         = rd_u32(sec + 32);
    uint32_t fat_sz_32       = rd_u32(sec + 36);

    if (img->bytes_per_sector == 0 || img->bytes_per_sector > SDIMG_SECTOR_MAX) return false;
    if (img->sectors_per_cluster == 0) return false;
    if (img->num_fats == 0) return false;
    if (img->reserved_sectors == 0) return false;

    img->total_sectors   = total16 ? total16 : total32;
    img->sectors_per_fat = fat_sz_16 ? fat_sz_16 : fat_sz_32;
    img->bytes_per_cluster = (uint32_t)img->bytes_per_sector * img->sectors_per_cluster;

    img->root_dir_sectors = ((uint32_t)img->root_entries * 32 +
                             img->bytes_per_sector - 1) / img->bytes_per_sector;
    img->fat_start_sector = img->reserved_sectors;
    img->root_dir_start_sector = img->fat_start_sector + img->num_fats * img->sectors_per_fat;
    img->data_start_sector     = img->root_dir_start_sector + img->root_dir_sectors;

    uint32_t data_sectors = img->total_sectors - img->data_start_sector;
    img->count_of_clusters = data_sectors / img->sectors_per_cluster;

    if (img->count_of_clusters < 4085u) {
        return false;
    } else if (img->count_of_clusters < 65525u) {
        img->fs_kind = SDIMG_FS_FAT16;
        img->root_cluster = 0;
    } else {
        img->fs_kind = SDIMG_FS_FAT32;
        img->root_cluster = rd_u32(sec + 44);
        img->root_dir_sectors = 0;
        img->data_start_sector = img->root_dir_start_sector;
        data_sectors = img->total_sectors - img->data_start_sector;
        img->count_of_clusters = data_sectors / img->sectors_per_cluster;
    }

    return true;
}

/* ─── handle allocation ──────────────────────────────────────────── */

static int alloc_file(loci_sdimg_t* img) {
    for (int i = 0; i < SDIMG_MAX_FILES; i++) {
        if (!img->files[i].used) return i;
    }
    return -EMFILE;
}
static int alloc_dir(loci_sdimg_t* img) {
    for (int i = 0; i < SDIMG_MAX_DIRS; i++) {
        if (!img->dirs[i].used) return i;
    }
    return -EMFILE;
}

/* ─── directory iteration ────────────────────────────────────────── */

static int load_dir_sector(loci_sdimg_t* img, sdimg_handle_t* h,
                           uint32_t entry_index, uint8_t* out_sector) {
    uint32_t bytes_off = entry_index * 32u;
    uint32_t sec_in_obj = bytes_off / img->bytes_per_sector;
    uint32_t off_in_sec = bytes_off % img->bytes_per_sector;
    uint32_t lba;

    if (img->fs_kind == SDIMG_FS_FAT16 && h->first_cluster == 0) {
        if (sec_in_obj >= img->root_dir_sectors) return -1;
        lba = img->root_dir_start_sector + sec_in_obj;
    } else {
        uint32_t cluster = h->first_cluster;
        uint32_t want_sec = sec_in_obj;
        while (want_sec >= img->sectors_per_cluster) {
            cluster = read_fat_entry(img, cluster);
            if (cluster == 0 || cluster == 0xFFFFFFFFu) return -1;
            want_sec -= img->sectors_per_cluster;
        }
        lba = cluster_first_sector(img, cluster) + want_sec;
    }
    if (!read_sector(img, lba, out_sector)) return -1;
    return (int)off_in_sec;
}

static bool extract_short_name(const uint8_t* e, char out[13],
                               bool* end_of_dir, bool keep_dotdot) {
    uint8_t first = e[0];
    if (first == 0x00) { *end_of_dir = true; return false; }
    *end_of_dir = false;
    if (first == 0xE5) return false;
    if ((e[11] & ATTR_LFN) == ATTR_LFN) return false;
    if (e[11] & ATTR_VOLUME_ID) return false;

    char name[9], ext[4];
    int n = 0, x = 0;
    for (int i = 0; i < 8; i++) if (e[i] != ' ') name[n++] = (char)e[i];
    name[n] = 0;
    for (int i = 8; i < 11; i++) if (e[i] != ' ') ext[x++] = (char)e[i];
    ext[x] = 0;
    if (first == 0x05) name[0] = (char)0xE5;

    if (!keep_dotdot && n > 0 && name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.'))) {
        return false;
    }

    if (x > 0) snprintf(out, 13, "%s.%s", name, ext);
    else       snprintf(out, 13, "%s", name);
    return true;
}

static int ci_strcmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = toupper((unsigned char)*a++);
        int cb = toupper((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static bool dir_find_child(loci_sdimg_t* img, sdimg_handle_t* dir,
                           const char* name,
                           uint32_t* out_cluster, uint32_t* out_size, bool* out_is_dir) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    uint32_t cached_sec = 0xFFFFFFFFu;
    int off = -1;

    for (uint32_t i = 0; ; i++) {
        uint32_t bytes_off = i * 32u;
        uint32_t sec_in_obj = bytes_off / img->bytes_per_sector;
        if (cached_sec != sec_in_obj) {
            off = load_dir_sector(img, dir, i, sec);
            if (off < 0) return false;
            cached_sec = sec_in_obj;
        } else {
            off = (int)(bytes_off % img->bytes_per_sector);
        }
        const uint8_t* e = sec + off;
        char raw[13];
        bool eod;
        if (!extract_short_name(e, raw, &eod, true)) {
            if (eod) return false;
            continue;
        }
        if (ci_strcmp(raw, name) == 0) {
            uint16_t hi = rd_u16(e + 20);
            uint16_t lo = rd_u16(e + 26);
            *out_cluster = ((uint32_t)hi << 16) | lo;
            *out_size    = rd_u32(e + 28);
            *out_is_dir  = (e[11] & ATTR_DIRECTORY) != 0;
            return true;
        }
    }
}

static void init_root_handle(loci_sdimg_t* img, sdimg_handle_t* h) {
    memset(h, 0, sizeof(*h));
    h->is_dir = true;
    if (img->fs_kind == SDIMG_FS_FAT32) {
        h->first_cluster = img->root_cluster;
    } else {
        h->first_cluster = 0;
    }
    h->cursor = 0;
}

static bool resolve_path(loci_sdimg_t* img, const char* path,
                         uint32_t* out_cluster, uint32_t* out_size, bool* out_is_dir) {
    while (*path == '/' || *path == '\\') path++;
    if (*path == 0) {
        *out_cluster = (img->fs_kind == SDIMG_FS_FAT32) ? img->root_cluster : 0;
        *out_size = 0;
        *out_is_dir = true;
        return true;
    }

    sdimg_handle_t cur;
    init_root_handle(img, &cur);
    uint32_t cluster = cur.first_cluster;
    bool is_dir = true;
    uint32_t size = 0;

    char comp[64];
    while (*path) {
        size_t n = 0;
        while (*path && *path != '/' && *path != '\\' && n + 1 < sizeof(comp)) {
            comp[n++] = *path++;
        }
        comp[n] = 0;
        while (*path == '/' || *path == '\\') path++;
        if (n == 0) continue;
        if (!is_dir) return false;

        sdimg_handle_t dir;
        memset(&dir, 0, sizeof(dir));
        dir.is_dir = true;
        dir.first_cluster = cluster;

        if (!dir_find_child(img, &dir, comp, &cluster, &size, &is_dir)) {
            return false;
        }
    }

    *out_cluster = cluster;
    *out_size = size;
    *out_is_dir = is_dir;
    return true;
}

/* ─── low-level write helpers (Sprint 34ap) ──────────────────────── */

static void wr_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static bool write_sector(loci_sdimg_t* img, uint32_t lba, const uint8_t* in) {
    if (img->read_only) { errno = EROFS; return false; }
    if (fseek(img->fp, (long)lba * img->bytes_per_sector, SEEK_SET) != 0) return false;
    size_t n = fwrite(in, 1, img->bytes_per_sector, img->fp);
    return n == img->bytes_per_sector;
}

static bool write_fat_entry(loci_sdimg_t* img, uint32_t cluster, uint32_t value) {
    uint32_t entry_size = (img->fs_kind == SDIMG_FS_FAT32) ? 4u : 2u;
    uint32_t offset = cluster * entry_size;
    uint32_t sec_off = offset % img->bytes_per_sector;
    uint32_t sec_rel = offset / img->bytes_per_sector;

    uint8_t sec[SDIMG_SECTOR_MAX];
    uint32_t lba0 = img->fat_start_sector + sec_rel;
    if (!read_sector(img, lba0, sec)) return false;

    if (img->fs_kind == SDIMG_FS_FAT32) {
        uint32_t cur = rd_u32(sec + sec_off);
        cur = (cur & 0xF0000000u) | (value & 0x0FFFFFFFu);
        wr_u32(sec + sec_off, cur);
    } else {
        wr_u16(sec + sec_off, (uint16_t)value);
    }

    for (uint32_t i = 0; i < img->num_fats; i++) {
        uint32_t lba = img->fat_start_sector + i * img->sectors_per_fat + sec_rel;
        if (!write_sector(img, lba, sec)) return false;
    }
    return true;
}

static uint32_t alloc_free_cluster(loci_sdimg_t* img, uint32_t start_hint) {
    if (start_hint < 2) start_hint = 2;
    uint32_t end = 2 + img->count_of_clusters;
    for (uint32_t c = start_hint; c < end; c++) {
        if (read_fat_entry(img, c) == 0) {
            uint32_t eoc = (img->fs_kind == SDIMG_FS_FAT32) ? 0x0FFFFFFFu : 0xFFFFu;
            if (!write_fat_entry(img, c, eoc)) return 0;
            return c;
        }
    }
    for (uint32_t c = 2; c < start_hint; c++) {
        if (read_fat_entry(img, c) == 0) {
            uint32_t eoc = (img->fs_kind == SDIMG_FS_FAT32) ? 0x0FFFFFFFu : 0xFFFFu;
            if (!write_fat_entry(img, c, eoc)) return 0;
            return c;
        }
    }
    return 0;
}

static bool free_cluster_chain(loci_sdimg_t* img, uint32_t first_cluster) {
    uint32_t c = first_cluster;
    while (c >= 2 && c < 2 + img->count_of_clusters) {
        uint32_t next = read_fat_entry(img, c);
        if (!write_fat_entry(img, c, 0)) return false;
        if (next == 0xFFFFFFFFu || next < 2) break;
        c = next;
    }
    return true;
}

static uint32_t extend_chain(loci_sdimg_t* img, uint32_t last_cluster) {
    uint32_t newc = alloc_free_cluster(img, last_cluster + 1);
    if (newc == 0) return 0;
    if (!write_fat_entry(img, last_cluster, newc)) return 0;
    return newc;
}

static bool find_dir_entry(loci_sdimg_t* img, sdimg_handle_t* parent,
                           const char* name,
                           uint32_t* out_lba, uint32_t* out_off) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    for (uint32_t i = 0; ; i++) {
        uint32_t bytes_off = i * 32u;
        uint32_t sec_in_obj = bytes_off / img->bytes_per_sector;
        uint32_t off_in_sec = bytes_off % img->bytes_per_sector;
        uint32_t lba;

        if (img->fs_kind == SDIMG_FS_FAT16 && parent->first_cluster == 0) {
            if (sec_in_obj >= img->root_dir_sectors) return false;
            lba = img->root_dir_start_sector + sec_in_obj;
        } else {
            uint32_t cluster = parent->first_cluster;
            uint32_t want = sec_in_obj;
            while (want >= img->sectors_per_cluster) {
                cluster = read_fat_entry(img, cluster);
                if (cluster == 0 || cluster == 0xFFFFFFFFu) return false;
                want -= img->sectors_per_cluster;
            }
            lba = cluster_first_sector(img, cluster) + want;
        }
        if (!read_sector(img, lba, sec)) return false;

        const uint8_t* e = sec + off_in_sec;
        char raw[13];
        bool eod;
        if (!extract_short_name(e, raw, &eod, true)) {
            if (eod) return false;
            continue;
        }
        if (ci_strcmp(raw, name) == 0) {
            *out_lba = lba;
            *out_off = off_in_sec;
            return true;
        }
    }
}

static bool alloc_dir_entry(loci_sdimg_t* img, sdimg_handle_t* parent,
                            uint32_t* out_lba, uint32_t* out_off) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    uint32_t cached_sec = 0xFFFFFFFFu;
    for (uint32_t i = 0; ; i++) {
        uint32_t bytes_off = i * 32u;
        uint32_t sec_in_obj = bytes_off / img->bytes_per_sector;
        uint32_t off_in_sec = bytes_off % img->bytes_per_sector;
        uint32_t lba;

        if (img->fs_kind == SDIMG_FS_FAT16 && parent->first_cluster == 0) {
            if (sec_in_obj >= img->root_dir_sectors) {
                errno = ENOSPC; return false;
            }
            lba = img->root_dir_start_sector + sec_in_obj;
        } else {
            uint32_t cluster = parent->first_cluster;
            uint32_t want = sec_in_obj;
            while (want >= img->sectors_per_cluster) {
                uint32_t nxt = read_fat_entry(img, cluster);
                if (nxt == 0xFFFFFFFFu) {
                    uint32_t newc = extend_chain(img, cluster);
                    if (newc == 0) { errno = ENOSPC; return false; }
                    uint8_t zero[SDIMG_SECTOR_MAX] = {0};
                    uint32_t base = cluster_first_sector(img, newc);
                    for (uint32_t s = 0; s < img->sectors_per_cluster; s++) {
                        if (!write_sector(img, base + s, zero)) return false;
                    }
                    nxt = newc;
                }
                cluster = nxt;
                want -= img->sectors_per_cluster;
            }
            lba = cluster_first_sector(img, cluster) + want;
        }
        if (cached_sec != lba) {
            if (!read_sector(img, lba, sec)) return false;
            cached_sec = lba;
        }
        uint8_t first = sec[off_in_sec];
        if (first == 0x00 || first == 0xE5) {
            *out_lba = lba;
            *out_off = off_in_sec;
            return true;
        }
    }
}

static bool split_parent(const char* path, char* parent_out, size_t parent_sz,
                         char* base_out, size_t base_sz) {
    while (*path == '/' || *path == '\\') path++;
    if (!*path) return false;
    const char* last_slash = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last_slash = p;
    }
    if (last_slash) {
        size_t n = (size_t)(last_slash - path);
        if (n >= parent_sz) return false;
        memcpy(parent_out, path, n);
        parent_out[n] = 0;
        strncpy(base_out, last_slash + 1, base_sz - 1);
        base_out[base_sz - 1] = 0;
    } else {
        parent_out[0] = 0;
        strncpy(base_out, path, base_sz - 1);
        base_out[base_sz - 1] = 0;
    }
    return base_out[0] != 0;
}

static bool to_fat_83(const char* in, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char* dot = strrchr(in, '.');
    size_t name_len = dot ? (size_t)(dot - in) : strlen(in);
    size_t ext_len  = dot ? strlen(dot + 1) : 0;
    if (name_len == 0 || name_len > 8 || ext_len > 3) return false;
    for (size_t i = 0; i < name_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' ||
            c == ':' || c == '"' || c == '*' || c == '?' || c == '<' ||
            c == '>' || c == '|' || c == ' ') return false;
        out[i] = (uint8_t)toupper(c);
    }
    for (size_t i = 0; i < ext_len; i++) {
        unsigned char c = (unsigned char)dot[1 + i];
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' ||
            c == ':' || c == '"' || c == '*' || c == '?' || c == '<' ||
            c == '>' || c == '|' || c == ' ') return false;
        out[8 + i] = (uint8_t)toupper(c);
    }
    return true;
}

static bool update_dir_entry(loci_sdimg_t* img, uint32_t lba, uint32_t off,
                             uint32_t first_cluster, uint32_t size_bytes) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    if (!read_sector(img, lba, sec)) return false;
    uint8_t* e = sec + off;
    wr_u16(e + 20, (uint16_t)((first_cluster >> 16) & 0xFFFF));
    wr_u16(e + 26, (uint16_t)(first_cluster & 0xFFFF));
    wr_u32(e + 28, size_bytes);
    return write_sector(img, lba, sec);
}

static bool write_new_entry(loci_sdimg_t* img, uint32_t lba, uint32_t off,
                            const uint8_t name83[11], uint8_t attrib,
                            uint32_t first_cluster, uint32_t size_bytes) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    if (!read_sector(img, lba, sec)) return false;
    uint8_t* e = sec + off;
    memset(e, 0, 32);
    memcpy(e, name83, 11);
    e[11] = attrib;
    wr_u16(e + 20, (uint16_t)((first_cluster >> 16) & 0xFFFF));
    wr_u16(e + 26, (uint16_t)(first_cluster & 0xFFFF));
    wr_u32(e + 28, size_bytes);
    return write_sector(img, lba, sec);
}

/* Mark the dir entry at (lba, off) as deleted (first byte 0xE5). */
static bool mark_entry_deleted(loci_sdimg_t* img, uint32_t lba, uint32_t off) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    if (!read_sector(img, lba, sec)) return false;
    sec[off] = 0xE5;
    return write_sector(img, lba, sec);
}

/* ─── public API ────────────────────────────────────────────────── */

loci_sdimg_t* loci_sdimg_open(const char* path) {
    if (!path) { errno = EINVAL; return NULL; }
    bool ro = false;
    FILE* fp = fopen(path, "rb+");
    if (!fp) {
        fp = fopen(path, "rb");
        ro = (fp != NULL);
    }
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); errno = EINVAL; return NULL; }

    loci_sdimg_t* img = calloc(1, sizeof(*img));
    if (!img) { fclose(fp); errno = ENOMEM; return NULL; }
    img->fp = fp;
    img->total_size = (uint32_t)sz;
    img->read_only = ro;

    if (!parse_bpb(img)) {
        fclose(fp);
        free(img);
        errno = EINVAL;
        return NULL;
    }
    return img;
}

void loci_sdimg_close(loci_sdimg_t* img) {
    if (!img) return;
    if (img->fp) fclose(img->fp);
    free(img);
}

const char* loci_sdimg_fs_label(const loci_sdimg_t* img) {
    if (!img) return "";
    return img->fs_kind == SDIMG_FS_FAT32 ? "FAT32" : "FAT16";
}

uint32_t loci_sdimg_total_size(const loci_sdimg_t* img) {
    return img ? img->total_size : 0;
}

static int fopen_existing_read(loci_sdimg_t* img, const char* path,
                               bool writable) {
    char parent_path[256], base[64];
    if (!split_parent(path, parent_path, sizeof(parent_path),
                      base, sizeof(base))) return -EINVAL;

    uint32_t parent_cluster, parent_size;
    bool parent_is_dir;
    if (!resolve_path(img, parent_path,
                      &parent_cluster, &parent_size, &parent_is_dir)) return -ENOENT;
    if (!parent_is_dir) return -ENOTDIR;

    sdimg_handle_t parent;
    memset(&parent, 0, sizeof(parent));
    parent.is_dir = true;
    parent.first_cluster = parent_cluster;

    uint32_t lba, off;
    if (!find_dir_entry(img, &parent, base, &lba, &off)) return -ENOENT;

    uint8_t sec[SDIMG_SECTOR_MAX];
    if (!read_sector(img, lba, sec)) return -EIO;
    const uint8_t* e = sec + off;
    if (e[11] & ATTR_DIRECTORY) return -EISDIR;
    uint16_t hi = rd_u16(e + 20);
    uint16_t lo = rd_u16(e + 26);
    uint32_t first_cluster = ((uint32_t)hi << 16) | lo;
    uint32_t size = rd_u32(e + 28);

    int slot = alloc_file(img);
    if (slot < 0) return slot;
    sdimg_handle_t* h = &img->files[slot];
    memset(h, 0, sizeof(*h));
    h->used = true;
    h->is_dir = false;
    h->writable = writable;
    h->first_cluster = first_cluster;
    h->size_bytes = size;
    h->current_cluster = first_cluster;
    h->cluster_byte_offset = 0;
    h->cursor = 0;
    h->dir_entry_lba = lba;
    h->dir_entry_off = off;
    return slot;
}

int loci_sdimg_fopen(loci_sdimg_t* img, const char* path) {
    if (!img || !path) return -EINVAL;
    return fopen_existing_read(img, path, false);
}

/* Create new file: resolve parent, alloc dir entry, write 8.3 entry
 * with first_cluster=0 size=0. Caller gets a writable handle. */
static int create_new_file(loci_sdimg_t* img, const char* path) {
    if (img->read_only) return -EROFS;
    char parent_path[256], base[64];
    if (!split_parent(path, parent_path, sizeof(parent_path),
                      base, sizeof(base))) return -EINVAL;

    uint8_t name83[11];
    if (!to_fat_83(base, name83)) return -EINVAL;

    uint32_t parent_cluster, parent_size;
    bool parent_is_dir;
    if (!resolve_path(img, parent_path,
                      &parent_cluster, &parent_size, &parent_is_dir)) return -ENOENT;
    if (!parent_is_dir) return -ENOTDIR;

    sdimg_handle_t parent;
    memset(&parent, 0, sizeof(parent));
    parent.is_dir = true;
    parent.first_cluster = parent_cluster;

    /* Refuse if it already exists. */
    uint32_t exist_lba, exist_off;
    if (find_dir_entry(img, &parent, base, &exist_lba, &exist_off)) return -EEXIST;

    uint32_t lba, off;
    if (!alloc_dir_entry(img, &parent, &lba, &off)) {
        return errno == ENOSPC ? -ENOSPC : -EIO;
    }
    if (!write_new_entry(img, lba, off, name83, ATTR_ARCHIVE, 0, 0)) return -EIO;

    int slot = alloc_file(img);
    if (slot < 0) return slot;
    sdimg_handle_t* h = &img->files[slot];
    memset(h, 0, sizeof(*h));
    h->used = true;
    h->is_dir = false;
    h->writable = true;
    h->first_cluster = 0;
    h->size_bytes = 0;
    h->current_cluster = 0;
    h->cluster_byte_offset = 0;
    h->cursor = 0;
    h->dir_entry_lba = lba;
    h->dir_entry_off = off;
    return slot;
}

int loci_sdimg_fopen_ex(loci_sdimg_t* img, const char* path, int mode) {
    if (!img || !path) return -EINVAL;
    if (mode == 0) return fopen_existing_read(img, path, false);

    /* Mode 1 (W) or 2 (R+W): create-or-truncate. */
    if (img->read_only) return -EROFS;
    int existing = fopen_existing_read(img, path, true);
    if (existing >= 0) {
        /* Truncate: free chain, reset size. */
        sdimg_handle_t* h = &img->files[existing];
        if (h->first_cluster >= 2) {
            free_cluster_chain(img, h->first_cluster);
        }
        h->first_cluster = 0;
        h->size_bytes = 0;
        h->current_cluster = 0;
        h->cluster_byte_offset = 0;
        h->cursor = 0;
        update_dir_entry(img, h->dir_entry_lba, h->dir_entry_off, 0, 0);
        return existing;
    }
    if (existing != -ENOENT) return existing;
    return create_new_file(img, path);
}

int loci_sdimg_fclose(loci_sdimg_t* img, int fd) {
    if (!img || fd < 0 || fd >= SDIMG_MAX_FILES) return -EBADF;
    if (!img->files[fd].used) return -EBADF;
    img->files[fd].used = false;
    return 0;
}

static bool seek_to_offset(loci_sdimg_t* img, sdimg_handle_t* h, uint32_t off) {
    uint32_t cluster = h->first_cluster;
    uint32_t pos = 0;
    while (pos + img->bytes_per_cluster <= off) {
        cluster = read_fat_entry(img, cluster);
        if (cluster == 0 || cluster == 0xFFFFFFFFu) return false;
        pos += img->bytes_per_cluster;
    }
    h->current_cluster = cluster;
    h->cluster_byte_offset = off - pos;
    h->cursor = off;
    return true;
}

int loci_sdimg_fread(loci_sdimg_t* img, int fd, void* buf, uint16_t count) {
    if (!img || fd < 0 || fd >= SDIMG_MAX_FILES) return -EBADF;
    sdimg_handle_t* h = &img->files[fd];
    if (!h->used) return -EBADF;
    if (h->cursor >= h->size_bytes) return 0;

    uint32_t remaining_file = h->size_bytes - h->cursor;
    uint32_t to_read = count;
    if (to_read > remaining_file) to_read = remaining_file;
    if (to_read == 0) return 0;

    uint8_t* out = (uint8_t*)buf;
    uint32_t done = 0;
    while (done < to_read) {
        uint32_t sec_idx = cluster_first_sector(img, h->current_cluster)
                         + h->cluster_byte_offset / img->bytes_per_sector;
        uint32_t off_in_sec = h->cluster_byte_offset % img->bytes_per_sector;
        uint32_t chunk = img->bytes_per_sector - off_in_sec;
        if (chunk > to_read - done) chunk = to_read - done;

        uint8_t sec[SDIMG_SECTOR_MAX];
        if (!read_sector(img, sec_idx, sec)) return -EIO;
        memcpy(out + done, sec + off_in_sec, chunk);
        done += chunk;
        h->cluster_byte_offset += chunk;
        h->cursor += chunk;

        if (h->cluster_byte_offset >= img->bytes_per_cluster && done < to_read) {
            uint32_t next = read_fat_entry(img, h->current_cluster);
            if (next == 0 || next == 0xFFFFFFFFu) break;
            h->current_cluster = next;
            h->cluster_byte_offset = 0;
        }
    }
    return (int)done;
}

int loci_sdimg_fwrite(loci_sdimg_t* img, int fd,
                      const void* buf, uint16_t count) {
    if (!img || fd < 0 || fd >= SDIMG_MAX_FILES) return -EBADF;
    sdimg_handle_t* h = &img->files[fd];
    if (!h->used) return -EBADF;
    if (!h->writable) return -EACCES;
    if (img->read_only) return -EROFS;
    if (count == 0) return 0;

    /* Allocate first cluster if file is currently empty. */
    if (h->first_cluster < 2) {
        uint32_t c = alloc_free_cluster(img, 2);
        if (c == 0) return -ENOSPC;
        h->first_cluster = c;
        h->current_cluster = c;
        h->cluster_byte_offset = 0;
    }

    const uint8_t* in = (const uint8_t*)buf;
    uint32_t done = 0;
    while (done < count) {
        /* Read-modify-write the current sector. */
        uint32_t sec_idx = cluster_first_sector(img, h->current_cluster)
                         + h->cluster_byte_offset / img->bytes_per_sector;
        uint32_t off_in_sec = h->cluster_byte_offset % img->bytes_per_sector;
        uint32_t chunk = img->bytes_per_sector - off_in_sec;
        if (chunk > (uint32_t)count - done) chunk = (uint32_t)count - done;

        uint8_t sec[SDIMG_SECTOR_MAX];
        if (!read_sector(img, sec_idx, sec)) return (int)done > 0 ? (int)done : -EIO;
        memcpy(sec + off_in_sec, in + done, chunk);
        if (!write_sector(img, sec_idx, sec)) return (int)done > 0 ? (int)done : -EIO;

        done += chunk;
        h->cluster_byte_offset += chunk;
        h->cursor += chunk;
        if (h->cursor > h->size_bytes) h->size_bytes = h->cursor;

        if (h->cluster_byte_offset >= img->bytes_per_cluster && done < count) {
            uint32_t next = read_fat_entry(img, h->current_cluster);
            if (next == 0xFFFFFFFFu) {
                /* Extend chain by one cluster. */
                uint32_t newc = extend_chain(img, h->current_cluster);
                if (newc == 0) {
                    /* Out of space — finalize dir entry and return partial. */
                    update_dir_entry(img, h->dir_entry_lba, h->dir_entry_off,
                                     h->first_cluster, h->size_bytes);
                    return (int)done > 0 ? (int)done : -ENOSPC;
                }
                next = newc;
            }
            h->current_cluster = next;
            h->cluster_byte_offset = 0;
        }
    }
    /* Update dir entry size + first_cluster (in case file was empty). */
    update_dir_entry(img, h->dir_entry_lba, h->dir_entry_off,
                     h->first_cluster, h->size_bytes);
    return (int)done;
}

int32_t loci_sdimg_lseek(loci_sdimg_t* img, int fd, int32_t offset, uint8_t whence) {
    if (!img || fd < 0 || fd >= SDIMG_MAX_FILES) return -EBADF;
    sdimg_handle_t* h = &img->files[fd];
    if (!h->used) return -EBADF;
    int64_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = h->cursor; break;
        case 2: base = h->size_bytes; break;
        default: return -EINVAL;
    }
    int64_t target = base + offset;
    if (target < 0) return -EINVAL;
    if (target > h->size_bytes) target = h->size_bytes;
    if (target == 0) {
        h->current_cluster = h->first_cluster;
        h->cluster_byte_offset = 0;
        h->cursor = 0;
    } else if (!seek_to_offset(img, h, (uint32_t)target)) return -EIO;
    return (int32_t)h->cursor;
}

int loci_sdimg_opendir(loci_sdimg_t* img, const char* path) {
    if (!img || !path) return -EINVAL;
    uint32_t cluster, size;
    bool is_dir;
    if (!resolve_path(img, path, &cluster, &size, &is_dir)) return -ENOENT;
    if (!is_dir) return -ENOTDIR;
    int slot = alloc_dir(img);
    if (slot < 0) return slot;
    sdimg_handle_t* h = &img->dirs[slot];
    memset(h, 0, sizeof(*h));
    h->used = true;
    h->is_dir = true;
    h->first_cluster = cluster;
    h->cursor = 0;
    return slot;
}

int loci_sdimg_closedir(loci_sdimg_t* img, int dh) {
    if (!img || dh < 0 || dh >= SDIMG_MAX_DIRS) return -EBADF;
    if (!img->dirs[dh].used) return -EBADF;
    img->dirs[dh].used = false;
    return 0;
}

int loci_sdimg_readdir(loci_sdimg_t* img, int dh,
                       char name[64], uint8_t* attrib, uint32_t* size) {
    if (!img || dh < 0 || dh >= SDIMG_MAX_DIRS) return -EBADF;
    sdimg_handle_t* h = &img->dirs[dh];
    if (!h->used) return -EBADF;

    uint8_t sec[SDIMG_SECTOR_MAX];
    while (1) {
        int off = load_dir_sector(img, h, h->cursor, sec);
        if (off < 0) {
            name[0] = 0;
            if (attrib) *attrib = 0;
            if (size)   *size = 0;
            return 0;
        }
        const uint8_t* e = sec + off;
        char raw[13];
        bool eod;
        bool ok = extract_short_name(e, raw, &eod, false);
        h->cursor++;
        if (eod) {
            name[0] = 0;
            if (attrib) *attrib = 0;
            if (size)   *size = 0;
            return 0;
        }
        if (!ok) continue;
        strncpy(name, raw, 63);
        name[63] = 0;
        if (attrib) *attrib = e[11];
        if (size)   *size   = rd_u32(e + 28);
        return 1;
    }
}

int loci_sdimg_unlink(loci_sdimg_t* img, const char* path) {
    if (!img || !path) return -EINVAL;
    if (img->read_only) return -EROFS;

    char parent_path[256], base[64];
    if (!split_parent(path, parent_path, sizeof(parent_path),
                      base, sizeof(base))) return -EINVAL;

    uint32_t parent_cluster, parent_size;
    bool parent_is_dir;
    if (!resolve_path(img, parent_path,
                      &parent_cluster, &parent_size, &parent_is_dir)) return -ENOENT;

    sdimg_handle_t parent;
    memset(&parent, 0, sizeof(parent));
    parent.is_dir = true;
    parent.first_cluster = parent_cluster;

    uint32_t lba, off;
    if (!find_dir_entry(img, &parent, base, &lba, &off)) return -ENOENT;

    uint8_t sec[SDIMG_SECTOR_MAX];
    if (!read_sector(img, lba, sec)) return -EIO;
    const uint8_t* e = sec + off;
    if (e[11] & ATTR_DIRECTORY) return -EISDIR;
    uint16_t hi = rd_u16(e + 20);
    uint16_t lo = rd_u16(e + 26);
    uint32_t first_cluster = ((uint32_t)hi << 16) | lo;

    if (first_cluster >= 2) free_cluster_chain(img, first_cluster);
    if (!mark_entry_deleted(img, lba, off)) return -EIO;
    return 0;
}

int loci_sdimg_rename(loci_sdimg_t* img, const char* old_path,
                      const char* new_path) {
    if (!img || !old_path || !new_path) return -EINVAL;
    if (img->read_only) return -EROFS;

    char old_parent[256], old_base[64];
    char new_parent[256], new_base[64];
    if (!split_parent(old_path, old_parent, sizeof(old_parent),
                      old_base, sizeof(old_base))) return -EINVAL;
    if (!split_parent(new_path, new_parent, sizeof(new_parent),
                      new_base, sizeof(new_base))) return -EINVAL;

    uint8_t new83[11];
    if (!to_fat_83(new_base, new83)) return -EINVAL;

    /* Locate the old entry to capture cluster/size/attrib. */
    uint32_t old_p_cluster, old_p_size;
    bool old_p_is_dir;
    if (!resolve_path(img, old_parent, &old_p_cluster, &old_p_size, &old_p_is_dir))
        return -ENOENT;
    sdimg_handle_t op;
    memset(&op, 0, sizeof(op));
    op.is_dir = true;
    op.first_cluster = old_p_cluster;
    uint32_t old_lba, old_off;
    if (!find_dir_entry(img, &op, old_base, &old_lba, &old_off)) return -ENOENT;
    uint8_t sec[SDIMG_SECTOR_MAX];
    if (!read_sector(img, old_lba, sec)) return -EIO;
    uint8_t* e = sec + old_off;
    uint8_t attrib = e[11];
    uint16_t hi = rd_u16(e + 20);
    uint16_t lo = rd_u16(e + 26);
    uint32_t first_cluster = ((uint32_t)hi << 16) | lo;
    uint32_t size = rd_u32(e + 28);

    /* Same parent → just rewrite the name in place. */
    if (strcmp(old_parent, new_parent) == 0) {
        memcpy(e, new83, 11);
        return write_sector(img, old_lba, sec) ? 0 : -EIO;
    }

    /* Different parent → alloc new entry, then delete old. */
    uint32_t new_p_cluster, new_p_size;
    bool new_p_is_dir;
    if (!resolve_path(img, new_parent, &new_p_cluster, &new_p_size, &new_p_is_dir))
        return -ENOENT;
    sdimg_handle_t np;
    memset(&np, 0, sizeof(np));
    np.is_dir = true;
    np.first_cluster = new_p_cluster;
    uint32_t exists_lba, exists_off;
    if (find_dir_entry(img, &np, new_base, &exists_lba, &exists_off)) return -EEXIST;
    uint32_t new_lba, new_off;
    if (!alloc_dir_entry(img, &np, &new_lba, &new_off)) {
        return errno == ENOSPC ? -ENOSPC : -EIO;
    }
    if (!write_new_entry(img, new_lba, new_off, new83, attrib,
                         first_cluster, size)) return -EIO;
    if (!mark_entry_deleted(img, old_lba, old_off)) return -EIO;
    return 0;
}

int loci_sdimg_mkdir(loci_sdimg_t* img, const char* path) {
    if (!img || !path) return -EINVAL;
    if (img->read_only) return -EROFS;

    char parent_path[256], base[64];
    if (!split_parent(path, parent_path, sizeof(parent_path),
                      base, sizeof(base))) return -EINVAL;
    uint8_t name83[11];
    if (!to_fat_83(base, name83)) return -EINVAL;

    uint32_t parent_cluster, parent_size;
    bool parent_is_dir;
    if (!resolve_path(img, parent_path,
                      &parent_cluster, &parent_size, &parent_is_dir)) return -ENOENT;

    sdimg_handle_t parent;
    memset(&parent, 0, sizeof(parent));
    parent.is_dir = true;
    parent.first_cluster = parent_cluster;

    uint32_t exist_lba, exist_off;
    if (find_dir_entry(img, &parent, base, &exist_lba, &exist_off)) return -EEXIST;

    /* Allocate cluster for the new directory and zero-init it. */
    uint32_t newc = alloc_free_cluster(img, 2);
    if (newc == 0) return -ENOSPC;
    uint8_t zero[SDIMG_SECTOR_MAX] = {0};
    uint32_t base_lba = cluster_first_sector(img, newc);
    for (uint32_t s = 0; s < img->sectors_per_cluster; s++) {
        if (!write_sector(img, base_lba + s, zero)) return -EIO;
    }

    /* Write "." and ".." entries in the new directory's first sector. */
    uint8_t sec[SDIMG_SECTOR_MAX] = {0};
    uint8_t dot[11], dotdot[11];
    memset(dot, ' ', 11); dot[0] = '.';
    memset(dotdot, ' ', 11); dotdot[0] = '.'; dotdot[1] = '.';
    uint8_t* e = sec;
    memcpy(e, dot, 11);
    e[11] = ATTR_DIRECTORY;
    wr_u16(e + 26, (uint16_t)(newc & 0xFFFF));
    wr_u16(e + 20, (uint16_t)((newc >> 16) & 0xFFFF));
    e += 32;
    memcpy(e, dotdot, 11);
    e[11] = ATTR_DIRECTORY;
    /* ".." cluster points to parent (0 = root for FAT16 convention). */
    uint32_t ppc = (img->fs_kind == SDIMG_FS_FAT16 && parent_cluster == 0)
                   ? 0 : parent_cluster;
    wr_u16(e + 26, (uint16_t)(ppc & 0xFFFF));
    wr_u16(e + 20, (uint16_t)((ppc >> 16) & 0xFFFF));
    if (!write_sector(img, base_lba, sec)) return -EIO;

    /* Add entry in parent directory. */
    uint32_t lba, off;
    if (!alloc_dir_entry(img, &parent, &lba, &off)) {
        return errno == ENOSPC ? -ENOSPC : -EIO;
    }
    if (!write_new_entry(img, lba, off, name83, ATTR_DIRECTORY, newc, 0))
        return -EIO;
    return 0;
}

int loci_sdimg_sync(loci_sdimg_t* img) {
    if (!img || !img->fp) return -EINVAL;
    return fflush(img->fp) == 0 ? 0 : -EIO;
}
