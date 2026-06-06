/* LOCI SD raw image backend (Sprint 34ao).
 *
 * Read-only FAT16/FAT32 reader operating on a raw .img file. Activated
 * via --loci-sdimg PATH (mutually exclusive with --loci-flash). All LOCI
 * file ops (open/read/lseek/close/opendir/readdir/closedir) delegate to
 * this backend instead of POSIX when loci->sdimg is non-NULL.
 *
 * Limitations of this initial implementation:
 *   - Read-only (write ops return EACCES).
 *   - FAT12 not supported (FAT16 / FAT32 only — auto-detected by cluster
 *     count per Microsoft FAT specification).
 *   - 8.3 short names only (LFN entries are skipped).
 *   - No MBR partitioning: image must start with a BPB at sector 0
 *     (i.e. a "superfloppy" / partition-less image).
 *
 * Mirrors the interface used by op_open/op_read_xstack/op_lseek/op_close
 * and op_opendir/op_readdir/op_closedir in src/io/loci.c.
 */
#ifndef PHOSPHORIC_LOCI_SDIMG_H
#define PHOSPHORIC_LOCI_SDIMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loci_sdimg_s loci_sdimg_t;

/* Open a raw FAT16/FAT32 image file. Returns NULL on error (set errno). */
loci_sdimg_t* loci_sdimg_open(const char* path);

/* Close the image and release all internal handles. Safe on NULL. */
void loci_sdimg_close(loci_sdimg_t* img);

/* Open a file by path (relative, forward slashes). Returns a non-negative
 * handle on success, or -errno on failure (use POSIX-ish ENOENT/EACCES/EIO). */
int  loci_sdimg_fopen(loci_sdimg_t* img, const char* path);

/* Close a file handle. Returns 0 on success, -EBADF on bad handle. */
int  loci_sdimg_fclose(loci_sdimg_t* img, int fd);

/* Read up to count bytes. Returns bytes read (0 on EOF), or -errno. */
int  loci_sdimg_fread(loci_sdimg_t* img, int fd, void* buf, uint16_t count);

/* Seek. whence: 0=SET, 1=CUR, 2=END. Returns new offset, or -errno. */
int32_t loci_sdimg_lseek(loci_sdimg_t* img, int fd,
                         int32_t offset, uint8_t whence);

/* Open a directory by path. Returns handle or -errno. */
int  loci_sdimg_opendir(loci_sdimg_t* img, const char* path);

/* Read next entry. Fills name[64], *attrib, *size. Returns 1 if an entry
 * was read, 0 on end-of-dir, -errno on error. Skips "." and "..". */
int  loci_sdimg_readdir(loci_sdimg_t* img, int dh,
                        char name[64], uint8_t* attrib, uint32_t* size);

/* Close directory handle. */
int  loci_sdimg_closedir(loci_sdimg_t* img, int dh);

/* Diagnostics. */
const char* loci_sdimg_fs_label(const loci_sdimg_t* img);   /* "FAT16"/"FAT32" */
uint32_t    loci_sdimg_total_size(const loci_sdimg_t* img);

#ifdef __cplusplus
}
#endif

#endif /* PHOSPHORIC_LOCI_SDIMG_H */
