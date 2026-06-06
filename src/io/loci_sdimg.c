/* LOCI SD raw image backend — read-only FAT16/32 reader. See header.
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
    uint32_t first_cluster;       /* 0 for FAT16 fixed root */
    uint32_t size_bytes;          /* 0 for directories */
    uint32_t cursor;              /* byte offset for files, entry index for dirs */
    uint32_t current_cluster;     /* cached cluster while walking */
    uint32_t cluster_byte_offset; /* current pos inside current_cluster */
} sdimg_handle_t;

struct loci_sdimg_s {
    FILE*      fp;
    sdimg_fs_t fs_kind;
    uint32_t   total_size;

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
    /* Bootstrap read: we don't know bytes_per_sector yet — read 512 max. */
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

    /* Root dir layout (FAT16): RootEntCnt fixed, located after FATs. */
    img->root_dir_sectors = ((uint32_t)img->root_entries * 32 +
                             img->bytes_per_sector - 1) / img->bytes_per_sector;
    img->fat_start_sector = img->reserved_sectors;
    img->root_dir_start_sector = img->fat_start_sector + img->num_fats * img->sectors_per_fat;
    img->data_start_sector     = img->root_dir_start_sector + img->root_dir_sectors;

    uint32_t data_sectors = img->total_sectors - img->data_start_sector;
    img->count_of_clusters = data_sectors / img->sectors_per_cluster;

    /* Microsoft FAT type determination (FAT spec): purely by cluster count. */
    if (img->count_of_clusters < 4085u) {
        return false;  /* FAT12 unsupported */
    } else if (img->count_of_clusters < 65525u) {
        img->fs_kind = SDIMG_FS_FAT16;
        img->root_cluster = 0;
    } else {
        img->fs_kind = SDIMG_FS_FAT32;
        img->root_cluster = rd_u32(sec + 44);
        img->root_dir_sectors = 0;
        /* For FAT32, recompute data_start (root dir is in data area). */
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

/* Load the sector containing the Nth dir entry for the given handle.
 * Returns the byte offset into `out_sector` and the sector index, or
 * -1 on end-of-dir / error. */
static int load_dir_sector(loci_sdimg_t* img, sdimg_handle_t* h,
                           uint32_t entry_index, uint8_t* out_sector) {
    uint32_t bytes_off = entry_index * 32u;
    uint32_t sec_in_obj = bytes_off / img->bytes_per_sector;
    uint32_t off_in_sec = bytes_off % img->bytes_per_sector;
    uint32_t lba;

    if (img->fs_kind == SDIMG_FS_FAT16 && h->first_cluster == 0) {
        /* Fixed-size root. */
        if (sec_in_obj >= img->root_dir_sectors) return -1;
        lba = img->root_dir_start_sector + sec_in_obj;
    } else {
        /* Walk cluster chain to find the cluster holding this sector. */
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

/* Extract 8.3 short name from a 32-byte dir entry into normalized "NAME.EXT".
 * Returns false if this entry is unusable (free, deleted, LFN, volume, or "."/".."). */
static bool extract_short_name(const uint8_t* e, char out[13],
                               bool* end_of_dir, bool keep_dotdot) {
    uint8_t first = e[0];
    if (first == 0x00) { *end_of_dir = true; return false; }
    *end_of_dir = false;
    if (first == 0xE5) return false;                              /* deleted */
    if ((e[11] & ATTR_LFN) == ATTR_LFN) return false;             /* LFN slot */
    if (e[11] & ATTR_VOLUME_ID) return false;                     /* volume label */

    /* Build "name.ext" */
    char name[9], ext[4];
    int n = 0, x = 0;
    for (int i = 0; i < 8; i++) if (e[i] != ' ') name[n++] = (char)e[i];
    name[n] = 0;
    for (int i = 8; i < 11; i++) if (e[i] != ' ') ext[x++] = (char)e[i];
    ext[x] = 0;
    if (first == 0x05) name[0] = (char)0xE5;  /* spec quirk */

    if (!keep_dotdot && n > 0 && name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.'))) {
        return false;
    }

    if (x > 0) snprintf(out, 13, "%s.%s", name, ext);
    else       snprintf(out, 13, "%s", name);
    return true;
}

/* Case-insensitive compare. */
static int ci_strcmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = toupper((unsigned char)*a++);
        int cb = toupper((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Find a child entry in a directory by name (case-insensitive 8.3 only).
 * Fills *out_cluster, *out_size, *out_is_dir. Returns true if found. */
static bool dir_find_child(loci_sdimg_t* img, sdimg_handle_t* dir,
                           const char* name,
                           uint32_t* out_cluster, uint32_t* out_size, bool* out_is_dir) {
    uint8_t sec[SDIMG_SECTOR_MAX];
    uint32_t cached_sec = 0xFFFFFFFFu;
    int idx = -1;
    int off = -1;
    (void)idx;

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

/* Initialize a directory handle to the volume root. */
static void init_root_handle(loci_sdimg_t* img, sdimg_handle_t* h) {
    memset(h, 0, sizeof(*h));
    h->is_dir = true;
    if (img->fs_kind == SDIMG_FS_FAT32) {
        h->first_cluster = img->root_cluster;
    } else {
        h->first_cluster = 0;  /* sentinel for fixed root */
    }
    h->cursor = 0;
}

/* Resolve a slash-separated path to a (cluster, size, is_dir) tuple.
 * Empty path or "/" → root directory. */
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

/* ─── public API ────────────────────────────────────────────────── */

loci_sdimg_t* loci_sdimg_open(const char* path) {
    if (!path) { errno = EINVAL; return NULL; }
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); errno = EINVAL; return NULL; }

    loci_sdimg_t* img = calloc(1, sizeof(*img));
    if (!img) { fclose(fp); errno = ENOMEM; return NULL; }
    img->fp = fp;
    img->total_size = (uint32_t)sz;

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

int loci_sdimg_fopen(loci_sdimg_t* img, const char* path) {
    if (!img || !path) return -EINVAL;
    uint32_t cluster, size;
    bool is_dir;
    if (!resolve_path(img, path, &cluster, &size, &is_dir)) return -ENOENT;
    if (is_dir) return -EISDIR;
    int slot = alloc_file(img);
    if (slot < 0) return slot;
    sdimg_handle_t* h = &img->files[slot];
    memset(h, 0, sizeof(*h));
    h->used = true;
    h->is_dir = false;
    h->first_cluster = cluster;
    h->size_bytes = size;
    h->current_cluster = cluster;
    h->cluster_byte_offset = 0;
    h->cursor = 0;
    return slot;
}

int loci_sdimg_fclose(loci_sdimg_t* img, int fd) {
    if (!img || fd < 0 || fd >= SDIMG_MAX_FILES) return -EBADF;
    if (!img->files[fd].used) return -EBADF;
    img->files[fd].used = false;
    return 0;
}

/* Advance file handle to the cluster containing the given byte offset,
 * walking from the start (simple but always correct). */
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
    if (!seek_to_offset(img, h, (uint32_t)target)) return -EIO;
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
