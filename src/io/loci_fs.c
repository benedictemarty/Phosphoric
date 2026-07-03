/**
 * @file loci_fs.c
 * @brief LOCI File I/O — open/close/read/write/lseek/unlink/rename,
 *        SDIMG backend dispatch, dir API (opendir/closedir/readdir/mkdir),
 *        getcwd / mount / umount / uname (Sprint 34aa-ac, 34ao-ap).
 *
 * Sprint 34c R4 : mechanical split of loci.c.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

#include "emulator.h"               /* EMU_VERSION for op_uname release */
#include "io/loci.h"
#include "io/loci_internal.h"
#include "io/loci_sdimg.h"
#include "utils/logging.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "utils/oscompat.h"

/* ─── helpers ─────────────────────────────────────────────────── */

/* Host filesystem error → LOCI errno. The backend plays the SD/FAT card
 * of the real LOCI, so filesystem errors carry the firmware's FatFS
 * encoding (32 + FRESULT, api.h API_EFATFS); only API-level conditions
 * (bad fd, slots full...) use the low 1-18 codes — like the firmware. */
uint16_t map_errno(int e) {
    switch (e) {
        case ENOENT:    return LOCI_EFATFS(LOCI_FR_NO_FILE);
        case ENOTDIR:   return LOCI_EFATFS(LOCI_FR_NO_PATH);
        case EACCES:    return LOCI_EFATFS(LOCI_FR_DENIED);
        case EPERM:     return LOCI_EFATFS(LOCI_FR_DENIED);
        case EEXIST:    return LOCI_EFATFS(LOCI_FR_EXIST);
        case EROFS:     return LOCI_EFATFS(LOCI_FR_WRITE_PROTECTED);
        case ENOMEM:    return LOCI_EFATFS(LOCI_FR_NOT_ENOUGH_CORE);
        case EBADF:     return LOCI_EBADF;
        case EINVAL:    return LOCI_EFATFS(LOCI_FR_INVALID_NAME);
        case ENOSPC:    return LOCI_EFATFS(LOCI_FR_DENIED);
        case EBUSY:     return LOCI_EFATFS(LOCI_FR_LOCKED);
        case EIO:       return LOCI_EFATFS(LOCI_FR_DISK_ERR);
        case ENOSYS:    return LOCI_ENOSYS;
        case EMFILE:    return LOCI_EFATFS(LOCI_FR_TOO_MANY_OPEN_FILES);
        case ENOTEMPTY: return LOCI_EFATFS(LOCI_FR_DENIED);
        default:        return LOCI_EUNKNOWN;
    }
}

bool resolve_path(loci_t* loci, const char* in,
                  char* out, size_t outsize) {
    const char* p = in;
    const char* root = loci->flash_root[0] ? loci->flash_root : ".";
    if (p[0] && p[1] == ':') {
        /* Firmware volume addressing: "0:" = internal storage (LFS),
         * "1:".."4:" = USB devices (FatFS volumes). A registered host
         * root (real USB key attached to the emulator) serves its
         * device number; other prefixes keep mapping to the flash root
         * (legacy behaviour, and "0:" is the flash root by definition). */
        if (p[0] >= '1' && p[0] <= '0' + LOCI_USB_DEV_MAX) {
            int idx = p[0] - '1';
            if (loci->usb_root[idx][0])
                root = loci->usb_root[idx];
        }
        p += 2;
    }
    while (*p == '/' || *p == '\\') p++;

    const char* c = p;
    while (*c) {
        const char* end = c;
        while (*end && *end != '/' && *end != '\\') end++;
        size_t len = (size_t)(end - c);
        if (len == 0) return false;
        if (len == 2 && c[0] == '.' && c[1] == '.') return false;
        c = end;
        while (*c == '/' || *c == '\\') c++;
    }

    int n = snprintf(out, outsize, "%s/%s", root, p);
    return n > 0 && (size_t)n < outsize;
}

static const char* fopen_mode_for(uint8_t flags) {
    int rw = flags & LOCI_O_RDWR;
    bool create = (flags & LOCI_O_CREAT) != 0;
    bool trunc  = (flags & LOCI_O_TRUNC) != 0;
    bool append = (flags & LOCI_O_APPEND) != 0;

    if (rw == 0) return "rb";
    if (append)  return rw == 1 ? "ab"  : "a+b";
    if (trunc)   return rw == 1 ? "wb"  : "w+b";
    if (create)  return rw == 1 ? "wb"  : "w+b";
    return rw == 1 ? "r+b" : "r+b";
}

static bool pop_zstring_keep(loci_t* loci, char* out, size_t outsize) {
    if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) return false;
    size_t n = 0;
    uint16_t p = loci->xstack_ptr;
    bool saw_term = false;
    while (p < LOCI_XSTACK_SIZE && n + 1 < outsize) {
        uint8_t c = loci->xstack[p++];
        if (c == 0) { saw_term = true; break; }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    loci->xstack_ptr = p;
    return n > 0 || saw_term;
}

static bool pop_zstring(loci_t* loci, char* out, size_t outsize) {
    bool ok = pop_zstring_keep(loci, out, outsize);
    xstack_zero(loci);
    return ok && out[0] != '\0';
}

static int alloc_fd(loci_t* loci) {
    for (int i = 0; i < LOCI_FD_MAX; i++) {
        if (loci->fds[i] == NULL) return i;
    }
    return -1;
}

static FILE* fd_to_file(loci_t* loci, int fd) {
    int idx = fd - LOCI_FD_OFFSET;
    if (idx < 0 || idx >= LOCI_FD_MAX) return NULL;
    return (FILE*)loci->fds[idx];
}

/* ─── SDIMG backend dispatch (Sprint 34ao) ──────────────────────── */

/* SDIMG (FAT16 image) error → LOCI errno: the SDIMG IS the FAT card, so
 * filesystem errors use the firmware's FatFS encoding (32 + FRESULT). */
int sdimg_errno_to_loci(int neg_errno) {
    int e = neg_errno < 0 ? -neg_errno : neg_errno;
    switch (e) {
        case 0:        return 0;
        case ENOENT:   return LOCI_EFATFS(LOCI_FR_NO_FILE);
        case EACCES:   return LOCI_EFATFS(LOCI_FR_DENIED);
        case EISDIR:   return LOCI_EFATFS(LOCI_FR_DENIED);
        case ENOTDIR:  return LOCI_EFATFS(LOCI_FR_NO_PATH);
        case EBADF:    return LOCI_EBADF;
        case EINVAL:   return LOCI_EFATFS(LOCI_FR_INVALID_NAME);
        case EMFILE:   return LOCI_EFATFS(LOCI_FR_TOO_MANY_OPEN_FILES);
        case EIO:      return LOCI_EFATFS(LOCI_FR_DISK_ERR);
        default:       return LOCI_EUNKNOWN;
    }
}

bool sdimg_extract_to_temp(loci_t* loci, const char* sd_path,
                           char* out_host_path, size_t out_sz) {
    const char* p = sd_path;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;

    int fd = loci_sdimg_fopen((loci_sdimg_t*)loci->sdimg, p);
    if (fd < 0) {
        log_info("LOCI SDIMG extract: not found in image: '%s' (errno=%d)", p, -fd);
        return false;
    }
    const char* base = strrchr(p, '/');
    base = base ? base + 1 : p;
    int n = snprintf(out_host_path, out_sz, "/tmp/loci_%s_XXXXXX", base);
    if (n <= 0 || (size_t)n >= out_sz) {
        loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
        return false;
    }
    int tmpfd = mkstemp(out_host_path);
    if (tmpfd < 0) {
        loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
        return false;
    }
    FILE* fp = fdopen(tmpfd, "wb");
    if (!fp) {
        close(tmpfd);
        unlink(out_host_path);
        loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
        return false;
    }
    uint8_t buf[512];
    int br;
    while ((br = loci_sdimg_fread((loci_sdimg_t*)loci->sdimg, fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)br, fp);
    }
    fclose(fp);
    loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, fd);
    log_info("LOCI SDIMG extract: '%s' → '%s'", p, out_host_path);
    return true;
}

static void op_open_sdimg(loci_t* loci) {
    uint8_t flags = loci->regs[LOCI_REG_API_A];
    char path[260] = {0};
    pop_zstring(loci, path, sizeof(path));
    if (path[0] == 0) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }
    const char* p = path;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;

    int rw = flags & LOCI_O_RDWR;
    int mode = 0;
    if ((flags & (LOCI_O_CREAT | LOCI_O_TRUNC | LOCI_O_APPEND)) ||
        rw == 1 || rw == 3) {
        mode = (rw == 3) ? 2 : 1;
    }
    int slot = loci_sdimg_fopen_ex((loci_sdimg_t*)loci->sdimg, p, mode);
    if (slot < 0) {
        api_return_errno(loci, sdimg_errno_to_loci(slot));
        return;
    }
    loci->fds[slot] = loci;
    loci->fd_kind[slot] = 2;
    api_return_ax(loci, (uint16_t)(slot + LOCI_FD_OFFSET));
}

static void op_close_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    int slot = fd - LOCI_FD_OFFSET;
    if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    int r = loci_sdimg_fclose((loci_sdimg_t*)loci->sdimg, slot);
    loci->fds[slot] = NULL;
    loci->fd_kind[slot] = 0;
    if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
    api_return_ax(loci, 0);
}

static void op_read_xstack_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 2 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL); return;
    }
    uint16_t count = (uint16_t)loci->xstack[loci->xstack_ptr]
                   | ((uint16_t)loci->xstack[loci->xstack_ptr + 1] << 8);
    loci->xstack_ptr += 2;
    if (count == 0 || count > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL); return;
    }
    int slot = fd - LOCI_FD_OFFSET;
    if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    uint8_t buf[LOCI_XSTACK_SIZE];
    int br = loci_sdimg_fread((loci_sdimg_t*)loci->sdimg, slot, buf, count);
    if (br < 0) { api_return_errno(loci, sdimg_errno_to_loci(br)); return; }
    xstack_zero(loci);
    if (br > 0) {
        loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - br);
        memcpy(&loci->xstack[loci->xstack_ptr], buf, (size_t)br);
        xstack_sync(loci);
    }
    api_return_ax(loci, (uint16_t)br);
}

/* Firmware lseek convention (loci-firmware std.c:377, Sprint 36e):
 * the int8 whence sits on TOP of the xstack (pushed last), then the
 * int32 offset below it as a "short stack" — 0 to 4 bytes, little-endian
 * in xstack memory, sign-extended like api_pop_int32_end().
 * whence values: 0 = SEEK_CUR, 1 = SEEK_END, 2 = SEEK_SET. */
static bool pop_lseek_args(loci_t* loci, int32_t* offset, uint8_t* whence) {
    if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) return false;     /* no whence */
    *whence = loci->xstack[loci->xstack_ptr++];
    uint16_t avail = (uint16_t)(LOCI_XSTACK_SIZE - loci->xstack_ptr);
    if (avail > 4) return false;
    int32_t v = 0;
    if (avail > 0) {
        memcpy((uint8_t*)&v + (4 - avail), &loci->xstack[loci->xstack_ptr], avail);
        v >>= 8 * (4 - avail);                  /* arithmetic: sign-extend */
        loci->xstack_ptr += avail;
    }
    *offset = v;
    xstack_zero(loci);
    return true;
}

static void op_lseek_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    int32_t offset;
    uint8_t whence;
    if (!pop_lseek_args(loci, &offset, &whence)) {
        api_return_errno(loci, LOCI_EINVAL); return;
    }
    int slot = fd - LOCI_FD_OFFSET;
    if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    /* loci_sdimg_lseek() expects POSIX-style 0=SET/1=CUR/2=END. */
    uint8_t sw;
    switch (whence) {
        case 0: sw = 1; break;
        case 1: sw = 2; break;
        case 2: sw = 0; break;
        default: api_return_errno(loci, LOCI_EINVAL); return;
    }
    int32_t pos = loci_sdimg_lseek((loci_sdimg_t*)loci->sdimg, slot, offset, sw);
    if (pos < 0) { api_return_errno(loci, sdimg_errno_to_loci(pos)); return; }
    api_return_axsreg(loci, (uint32_t)pos);
}

static void op_opendir_sdimg(loci_t* loci, const char* path) {
    const char* p = path;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;
    int slot = loci_sdimg_opendir((loci_sdimg_t*)loci->sdimg, p);
    if (slot < 0) { api_return_errno(loci, sdimg_errno_to_loci(slot)); return; }
    loci->dirs[slot] = loci;
    loci->dir_kind[slot] = 2;
    api_return_ax(loci, (uint16_t)(slot + LOCI_DIR_OFFSET));
}

static void op_closedir_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    int slot = fd - LOCI_DIR_OFFSET;
    if (slot < 0 || slot >= LOCI_DIR_MAX || !loci->dirs[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    int r = loci_sdimg_closedir((loci_sdimg_t*)loci->sdimg, slot);
    loci->dirs[slot] = NULL;
    loci->dir_kind[slot] = 0;
    if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
    api_return_ax(loci, 0);
}

static void op_readdir_sdimg(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    int slot = fd - LOCI_DIR_OFFSET;
    if (slot < 0 || slot >= LOCI_DIR_MAX || !loci->dirs[slot]) {
        api_return_errno(loci, LOCI_EBADF); return;
    }
    char name[64] = {0};
    uint8_t attrib = 0;
    uint32_t size = 0;
    int r = loci_sdimg_readdir((loci_sdimg_t*)loci->sdimg, slot, name, &attrib, &size);
    if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }

    uint8_t dirent_buf[LOCI_DIRENT_SIZE] = {0};
    dirent_buf[0] = (uint8_t)(fd & 0xFF);
    dirent_buf[1] = (uint8_t)((fd >> 8) & 0xFF);
    if (r == 1) {
        size_t nl = strlen(name);
        if (nl > LOCI_DIR_NAME_LEN - 1) nl = LOCI_DIR_NAME_LEN - 1;
        memcpy(&dirent_buf[2], name, nl);
        dirent_buf[66] = (attrib & LOCI_AM_DIR) ? LOCI_AM_DIR : 0;
        memcpy(&dirent_buf[68], &size, 4);
    }
    xstack_zero(loci);
    loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE);
    memcpy(&loci->xstack[loci->xstack_ptr], dirent_buf, LOCI_DIRENT_SIZE);
    xstack_sync(loci);
    api_return_ax(loci, 0);
}

/* ─── op_open / op_close / op_read_xstack / op_write_xstack ────── */

void op_open(loci_t* loci) {
    if (loci->sdimg) { op_open_sdimg(loci); return; }
    uint8_t flags = loci->regs[LOCI_REG_API_A];
    char path[260];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }

    if ((flags & LOCI_O_CREAT) && (flags & LOCI_O_EXCL)) {
        if (access(host_path, F_OK) == 0) {
            api_return_errno(loci, LOCI_EEXIST);
            return;
        }
    }

    FILE* fp = fopen(host_path, fopen_mode_for(flags));
    if (!fp) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    int slot = alloc_fd(loci);
    if (slot < 0) {
        fclose(fp);
        api_return_errno(loci, LOCI_EMFILE);
        return;
    }
    loci->fds[slot] = fp;
    loci->fd_kind[slot] = 1;
    api_return_ax(loci, (uint16_t)(slot + LOCI_FD_OFFSET));
}

void op_close(loci_t* loci) {
    if (loci->sdimg) { op_close_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    fclose(fp);
    loci->fds[fd - LOCI_FD_OFFSET] = NULL;
    loci->fd_kind[fd - LOCI_FD_OFFSET] = 0;
    api_return_ax(loci, 0);
}

void op_read_xstack(loci_t* loci) {
    if (loci->sdimg) { op_read_xstack_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 2 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint16_t count = (uint16_t)loci->xstack[loci->xstack_ptr]
                   | ((uint16_t)loci->xstack[loci->xstack_ptr + 1] << 8);
    loci->xstack_ptr += 2;
    if (count == 0 || count > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    uint8_t buf[LOCI_XSTACK_SIZE];
    size_t br = fread(buf, 1, count, fp);
    xstack_zero(loci);
    if (br > 0) {
        loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - br);
        memcpy(&loci->xstack[loci->xstack_ptr], buf, br);
        xstack_sync(loci);
    }
    api_return_ax(loci, (uint16_t)br);
}

/* Firmware write_xstack convention (loci-firmware std.c std_api_write_xstack):
 * unlike read_xstack, NO explicit count is popped from the xstack -- the
 * byte count is simply how many bytes are currently pushed, i.e.
 * XSTACK_SIZE - xstack_ptr. The data occupies xstack[xstack_ptr..SIZE).
 * (xahmol, PR #19 — Sprint 36h conformance fix.) */
void op_write_xstack(loci_t* loci) {
    uint16_t count = (uint16_t)(LOCI_XSTACK_SIZE - loci->xstack_ptr);
    uint8_t *data = &loci->xstack[loci->xstack_ptr];

    if (loci->sdimg) {
        int fd = loci->regs[LOCI_REG_API_A];
        if (count == 0) {
            xstack_zero(loci);
            api_return_ax(loci, 0);
            return;
        }
        int slot = fd - LOCI_FD_OFFSET;
        if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
            api_return_errno(loci, LOCI_EBADF); return;
        }
        int bw = loci_sdimg_fwrite((loci_sdimg_t*)loci->sdimg, slot, data, count);
        xstack_zero(loci);
        if (bw < 0) { api_return_errno(loci, sdimg_errno_to_loci(bw)); return; }
        api_return_ax(loci, (uint16_t)bw);
        return;
    }

    int fd = loci->regs[LOCI_REG_API_A];
    if (count == 0) {
        xstack_zero(loci);
        api_return_ax(loci, 0);
        return;
    }
    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    size_t bw = fwrite(data, 1, count, fp);
    xstack_zero(loci);
    api_return_ax(loci, (uint16_t)bw);
}

void op_lseek(loci_t* loci) {
    if (loci->sdimg) { op_lseek_sdimg(loci); return; }
    int fd = loci->regs[LOCI_REG_API_A];
    int32_t offset;
    uint8_t whence;
    if (!pop_lseek_args(loci, &offset, &whence)) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }

    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    int hw;
    switch (whence) {
        case 0: hw = SEEK_CUR; break;
        case 1: hw = SEEK_END; break;
        case 2: hw = SEEK_SET; break;
        default:
            api_return_errno(loci, LOCI_EINVAL);
            return;
    }
    if (fseek(fp, offset, hw) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    long pos = ftell(fp);
    if (pos < 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    api_return_axsreg(loci, (uint32_t)pos);
}

void op_read_xram(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 4 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint16_t count;
    uint16_t xram_addr;
    memcpy(&count, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    memcpy(&xram_addr, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    xstack_zero(loci);

    if ((uint32_t)xram_addr + count > LOCI_XRAM_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }

    if (loci->sdimg) {
        int slot = fd - LOCI_FD_OFFSET;
        if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
            api_return_errno(loci, LOCI_EBADF); return;
        }
        int br = loci_sdimg_fread((loci_sdimg_t*)loci->sdimg, slot,
                                  &loci->xram[xram_addr], count);
        if (br < 0) { api_return_errno(loci, sdimg_errno_to_loci(br)); return; }
        refresh_rw0(loci);
        refresh_rw1(loci);
        api_return_axsreg(loci, (uint32_t)br);
        return;
    }

    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    size_t br = fread(&loci->xram[xram_addr], 1, count, fp);
    refresh_rw0(loci);
    refresh_rw1(loci);
    api_return_axsreg(loci, (uint32_t)br);
}

void op_write_xram(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (loci->xstack_ptr + 4 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    uint16_t count;
    uint16_t xram_addr;
    memcpy(&count, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    memcpy(&xram_addr, &loci->xstack[loci->xstack_ptr], 2);
    loci->xstack_ptr += 2;
    xstack_zero(loci);

    if ((uint32_t)xram_addr + count > LOCI_XRAM_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }

    if (loci->sdimg) {
        int slot = fd - LOCI_FD_OFFSET;
        if (slot < 0 || slot >= LOCI_FD_MAX || !loci->fds[slot]) {
            api_return_errno(loci, LOCI_EBADF); return;
        }
        int bw = loci_sdimg_fwrite((loci_sdimg_t*)loci->sdimg, slot,
                                   &loci->xram[xram_addr], count);
        if (bw < 0) { api_return_errno(loci, sdimg_errno_to_loci(bw)); return; }
        api_return_axsreg(loci, (uint32_t)bw);
        return;
    }

    FILE* fp = fd_to_file(loci, fd);
    if (!fp) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    size_t bw = fwrite(&loci->xram[xram_addr], 1, count, fp);
    api_return_axsreg(loci, (uint32_t)bw);
}

void op_getcwd(loci_t* loci) {
    uint8_t drive = loci->regs[LOCI_REG_API_A];
    if (drive == 255) {
        if      (loci->mnt_mounted[LOCI_MNT_ROM]) drive = LOCI_MNT_ROM;
        else if (loci->mnt_mounted[0])            drive = 0;
        else if (loci->mnt_mounted[LOCI_MNT_TAP]) drive = LOCI_MNT_TAP;
        else { api_return_errno(loci, LOCI_ENOENT); return; }
    }
    if (drive >= LOCI_MNT_MAX || !loci->mnt_mounted[drive]) {
        api_return_errno(loci, LOCI_ENOENT);
        return;
    }
    const char* path = loci->mnt_paths[drive];
    const char* slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path + 1) : strlen(path);
    if (len > 255) len = 255;

    xstack_zero(loci);
    uint8_t zero = 0;
    if (!xstack_push_n(loci, &zero, 1)) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    for (size_t i = len; i > 0; i--) {
        uint8_t c = (uint8_t)path[i - 1];
        if (!xstack_push_n(loci, &c, 1)) {
            api_return_errno(loci, LOCI_EINVAL);
            return;
        }
    }
    api_return_ax(loci, (uint16_t)len);
}

void op_mount(loci_t* loci) {
    uint8_t drive = loci->regs[LOCI_REG_API_A];
    char path[256];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    if (drive >= LOCI_MNT_MAX) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (loci->sdimg) {
        if (!sdimg_extract_to_temp(loci, path, host_path, sizeof(host_path))) {
            /* firmware: f_open failure inside mount → EFATFS encoding */
            api_return_errno(loci, LOCI_EFATFS(LOCI_FR_NO_FILE));
            return;
        }
    } else {
        if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
            api_return_errno(loci, LOCI_EACCES);
            return;
        }
        if (access(host_path, F_OK) != 0) {
            /* firmware: f_open failure inside mount → EFATFS encoding */
            api_return_errno(loci, LOCI_EFATFS(LOCI_FR_NO_FILE));
            return;
        }
    }
    if (drive == LOCI_MNT_TAP) {
        if (!loci_tap_open(loci, host_path)) {
            api_return_errno(loci, LOCI_EIO);
            return;
        }
        if (loci->tape_mount_cb) {
            loci->tape_mount_cb(loci->tape_mount_ctx, host_path);
        }
    }
    if (drive < 4) {
        if (!loci_dsk_open(loci, drive, host_path)) {
            api_return_errno(loci, LOCI_EIO);
            return;
        }
    }
    loci->mnt_mounted[drive] = true;
    strncpy(loci->mnt_paths[drive], path, sizeof(loci->mnt_paths[drive]) - 1);
    loci->mnt_paths[drive][sizeof(loci->mnt_paths[drive]) - 1] = '\0';
    api_return_ax(loci, 0);
}

void op_umount(loci_t* loci) {
    uint8_t drive = loci->regs[LOCI_REG_API_A];
    if (drive < LOCI_MNT_MAX) {
        if (drive == LOCI_MNT_TAP) loci_tap_close(loci);
        if (drive < 4)             loci_dsk_close(loci, drive);
        loci->mnt_mounted[drive] = false;
        loci->mnt_paths[drive][0] = '\0';
    }
    api_return_ax(loci, 0);
}

void op_unlink(loci_t* loci) {
    if (loci->sdimg) {
        char path[260] = {0};
        pop_zstring(loci, path, sizeof(path));
        if (path[0] == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        const char* p = path;
        if (p[0] && p[1] == ':') p += 2;
        while (*p == '/' || *p == '\\') p++;
        int r = loci_sdimg_unlink((loci_sdimg_t*)loci->sdimg, p);
        if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
        api_return_ax(loci, 0);
        return;
    }
    char path[260];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    /* FatFs's f_unlink() (the real LOCI firmware's UNLINK backend) removes
     * both files and empty directories; bare unlink() fails EISDIR on a
     * directory, so detect that case and use rmdir() instead.
     * (xahmol, PR #10.) */
    struct stat st;
    if (stat(host_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (rmdir(host_path) != 0) {
            api_return_errno(loci, map_errno(errno));
            return;
        }
        api_return_ax(loci, 0);
        return;
    }
    if (unlink(host_path) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    api_return_ax(loci, 0);
}

void op_rename(loci_t* loci) {
    if (loci->sdimg) {
        char new_path[260] = {0};
        char old_path[260] = {0};
        bool ok1 = pop_zstring_keep(loci, new_path, sizeof(new_path));
        bool ok2 = pop_zstring_keep(loci, old_path, sizeof(old_path));
        xstack_zero(loci);
        if (!ok1 || !ok2 || old_path[0] == 0 || new_path[0] == 0) {
            api_return_errno(loci, LOCI_EINVAL); return;
        }
        const char* p_old = old_path;
        const char* p_new = new_path;
        if (p_old[0] && p_old[1] == ':') p_old += 2;
        while (*p_old == '/' || *p_old == '\\') p_old++;
        if (p_new[0] && p_new[1] == ':') p_new += 2;
        while (*p_new == '/' || *p_new == '\\') p_new++;
        int r = loci_sdimg_rename((loci_sdimg_t*)loci->sdimg, p_old, p_new);
        if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
        api_return_ax(loci, 0);
        return;
    }
    char new_path[260];
    char old_path[260];
    bool ok1 = pop_zstring_keep(loci, new_path, sizeof(new_path));
    bool ok2 = pop_zstring_keep(loci, old_path, sizeof(old_path));
    xstack_zero(loci);
    if (!ok1 || !ok2 || new_path[0] == 0 || old_path[0] == 0) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_new[512], host_old[512];
    if (!resolve_path(loci, new_path, host_new, sizeof(host_new)) ||
        !resolve_path(loci, old_path, host_old, sizeof(host_old))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    if (rename(host_old, host_new) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    api_return_ax(loci, 0);
}

/* ─── Dir API + uname (Sprint 34ac) ───────────────────────────── */

static int alloc_dir(loci_t* loci) {
    for (int i = 0; i < LOCI_DIR_MAX; i++) {
        if (loci->dirs[i] == NULL) return i;
    }
    return -1;
}

static DIR* dir_fd_to_handle(loci_t* loci, int fd) {
    int idx = fd - LOCI_DIR_OFFSET;
    if (idx < 0 || idx >= LOCI_DIR_MAX) return NULL;
    return (DIR*)loci->dirs[idx];
}

void op_opendir(loci_t* loci) {
    char path[260] = {0};
    /* Firmware dir.c reads the xstack guard on an empty stack (0x00): an
     * empty/absent path means the DEVICE LIST, not a directory. */
    bool popped = pop_zstring(loci, path, sizeof(path));
    if (!popped || path[0] == '\0') {
        if (loci->dir_dev >= 0) {              /* single iterator, like firmware */
            api_return_errno(loci, LOCI_EMFILE);
            return;
        }
        loci->dir_dev = 0;
        api_return_ax(loci, 0);                /* fd = FD_OFFS_DEV = 0 */
        return;
    }
    if (loci->sdimg) { op_opendir_sdimg(loci, path); return; }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    DIR* d = opendir(host_path);
    if (!d) {
        /* f_opendir reports a missing directory as FR_NO_PATH */
        api_return_errno(loci, errno == ENOENT
                             ? LOCI_EFATFS(LOCI_FR_NO_PATH)
                             : map_errno(errno));
        return;
    }
    int slot = alloc_dir(loci);
    if (slot < 0) {
        closedir(d);
        api_return_errno(loci, LOCI_EMFILE);
        return;
    }
    loci->dirs[slot] = d;
    loci->dir_kind[slot] = 1;
    size_t pn = strlen(host_path);
    if (pn >= sizeof(loci->dirs_path[slot])) pn = sizeof(loci->dirs_path[slot]) - 1;
    memcpy(loci->dirs_path[slot], host_path, pn);
    loci->dirs_path[slot][pn] = '\0';
    api_return_ax(loci, (uint16_t)(slot + LOCI_DIR_OFFSET));
}

void op_closedir(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (fd == 0 && loci->dir_dev >= 0) {   /* device-list iterator */
        loci->dir_dev = -1;
        api_return_ax(loci, 0);
        return;
    }
    if (loci->sdimg) { op_closedir_sdimg(loci); return; }
    DIR* d = dir_fd_to_handle(loci, fd);
    if (!d) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }
    closedir(d);
    loci->dirs[fd - LOCI_DIR_OFFSET] = NULL;
    loci->dir_kind[fd - LOCI_DIR_OFFSET] = 0;
    loci->dirs_path[fd - LOCI_DIR_OFFSET][0] = '\0';
    api_return_ax(loci, 0);
}

/* readdir on the device-list iterator (fd 0). Firmware dir.c: entry 0 is
 * the internal storage, then one usb_get_status() line per mounted USB
 * device, then an empty name terminates the listing. AM_SYS attribute. */
static void op_readdir_dev(loci_t* loci) {
    uint8_t dirent_buf[LOCI_DIRENT_SIZE] = {0};
    char name[LOCI_DIR_NAME_LEN] = "";
    if (loci->dir_dev == 0) {
        snprintf(name, sizeof(name), "0: Internal storage [15MB]");
    } else if (loci->dir_dev - 1 < (int)loci->usb_dev_count) {
        snprintf(name, sizeof(name), "%s", loci->usb_dev[loci->dir_dev - 1]);
    }
    loci->dir_dev++;
    size_t nl = strlen(name);
    memcpy(&dirent_buf[2], name, nl);      /* d_fd = 0 already (dev) */
    dirent_buf[66] = LOCI_AM_SYS;          /* firmware: devices are "system" */
    xstack_zero(loci);
    loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE);
    memcpy(&loci->xstack[loci->xstack_ptr], dirent_buf, LOCI_DIRENT_SIZE);
    xstack_sync(loci);
    api_return_ax(loci, 0);
}

void op_readdir(loci_t* loci) {
    int fd = loci->regs[LOCI_REG_API_A];
    if (fd == 0 && loci->dir_dev >= 0) { op_readdir_dev(loci); return; }
    if (loci->sdimg) { op_readdir_sdimg(loci); return; }
    DIR* d = dir_fd_to_handle(loci, fd);
    if (!d) {
        api_return_errno(loci, LOCI_EBADF);
        return;
    }

    struct dirent* de = NULL;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".")  == 0) continue;
        if (strcmp(de->d_name, "..") == 0) continue;
        break;
    }

    uint8_t dirent_buf[LOCI_DIRENT_SIZE] = {0};
    dirent_buf[0] = (uint8_t)(fd & 0xFF);
    dirent_buf[1] = (uint8_t)((fd >> 8) & 0xFF);

    if (de) {
        size_t nl = strlen(de->d_name);
        if (nl > LOCI_DIR_NAME_LEN - 1) nl = LOCI_DIR_NAME_LEN - 1;
        memcpy(&dirent_buf[2], de->d_name, nl);
        dirent_buf[2 + nl] = '\0';

        uint8_t attrib = 0;
        struct stat st;
        char fullpath[768];
        const char* base = loci->dirs_path[fd - LOCI_DIR_OFFSET];
        if (!base[0]) base = loci->flash_root[0] ? loci->flash_root : ".";
        snprintf(fullpath, sizeof(fullpath), "%s/%s", base, de->d_name);
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            attrib = LOCI_AM_DIR;
        }
        dirent_buf[66] = attrib;
        dirent_buf[67] = 0;

        uint32_t sz = 0;
        if (stat(fullpath, &st) == 0) sz = (uint32_t)st.st_size;
        memcpy(&dirent_buf[68], &sz, 4);
    }

    xstack_zero(loci);
    loci->xstack_ptr = (uint16_t)(LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE);
    memcpy(&loci->xstack[loci->xstack_ptr], dirent_buf, LOCI_DIRENT_SIZE);
    xstack_sync(loci);
    api_return_ax(loci, 0);
}

void op_mkdir(loci_t* loci) {
    if (loci->sdimg) {
        char path[260] = {0};
        pop_zstring(loci, path, sizeof(path));
        if (path[0] == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        const char* p = path;
        if (p[0] && p[1] == ':') p += 2;
        while (*p == '/' || *p == '\\') p++;
        int r = loci_sdimg_mkdir((loci_sdimg_t*)loci->sdimg, p);
        if (r < 0) { api_return_errno(loci, sdimg_errno_to_loci(r)); return; }
        api_return_ax(loci, 0);
        return;
    }
    char path[260];
    if (!pop_zstring(loci, path, sizeof(path))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    char host_path[512];
    if (!resolve_path(loci, path, host_path, sizeof(host_path))) {
        api_return_errno(loci, LOCI_EACCES);
        return;
    }
    if (oscompat_mkdir(host_path, 0755) != 0) {
        api_return_errno(loci, map_errno(errno));
        return;
    }
    api_return_ax(loci, 0);
}

void op_uname(loci_t* loci) {
    static const char machine[25]  = "Phosphoric Emulator     ";
    static const char version[9]   = "0.1     ";
    static const char nodename[9]  = "oric    ";
    static const char sysname[17]  = "Phosphoric LOCI ";

    char release[9];
    snprintf(release, sizeof(release), "%-8s", EMU_VERSION);

    xstack_zero(loci);
    if (!xstack_push_n(loci, machine,  sizeof(machine))  ||
        !xstack_push_n(loci, version,  sizeof(version))  ||
        !xstack_push_n(loci, release,  sizeof(release))  ||
        !xstack_push_n(loci, nodename, sizeof(nodename)) ||
        !xstack_push_n(loci, sysname,  sizeof(sysname))) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    api_return_ax(loci, loci->xstack_ptr);
}
