/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_hw_probe.c
 * @brief Recognising a LOCI-USB: loci-usb protocol + LOCI firmware ('L' at
 *        $0319). See include/io/loci_hw_probe.h.
 * @author bmarty <bmarty@mailo.com>
 *
 * Built everywhere; the loci-usb client is only used when the binary contains
 * the LOCI-USB backend (LOCI_BE_HAS_HW, Makefile), LOCI_PROBE_NO_BACKEND otherwise.
 */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define _DEFAULT_SOURCE   /* readlink, DT_* */
#endif
#include "io/loci_hw_probe.h"
#include "io/loci.h"   /* LOCI_DSK_IO_ID */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef LOCI_BE_HAS_HW
#include "loci_usb_client.h"
#endif

#define LOCI_ID_CHAR 'L'

int loci_hw_port_user(const char* dev) {
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    (void)dev;
    return 0;
#else
    struct stat want;
    if (!dev || stat(dev, &want) != 0) return 0;
    DIR* proc = opendir("/proc");
    if (!proc) return 0;
    const int self = (int)getpid();
    int found = 0;
    struct dirent* p;
    while (!found && (p = readdir(proc)) != NULL) {
        const int pid = atoi(p->d_name);
        if (pid <= 0 || pid == self) continue;
        char fdpath[64];
        snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", pid);
        DIR* fds = opendir(fdpath);   /* other users' processes: unreadable, skipped */
        if (!fds) continue;
        struct dirent* f;
        while ((f = readdir(fds)) != NULL) {
            char lp[96];
            struct stat st;
            if (f->d_name[0] == '.') continue;
            snprintf(lp, sizeof(lp), "%s/%.16s", fdpath, f->d_name);
            if (stat(lp, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == want.st_rdev) {
                found = pid;
                break;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return found;
#endif
}

#ifdef LOCI_BE_HAS_HW
/* PING already done by lup_open/lup_open_fd: reads the LOCI marker. */
static loci_probe_result_t check_id(lup_client_t* c, loci_probe_info_t* info) {
    uint8_t flags = 0;
    info->proto = c->proto;
    info->fw = c->fw;
    info->caps = c->caps;
    if (lup_rd(c, LOCI_DSK_IO_ID, &info->id, &flags) != 0) {
        snprintf(info->err, sizeof(info->err), "%s", lup_client_error(c));
        return LOCI_PROBE_NO_LINK;
    }
    return info->id == LOCI_ID_CHAR ? LOCI_PROBE_OK : LOCI_PROBE_NOT_LOCI;
}
#endif

loci_probe_result_t loci_hw_probe_fd(int fd, loci_probe_info_t* info) {
    memset(info, 0, sizeof(*info));
#ifdef LOCI_BE_HAS_HW
    lup_client_t c;
    if (lup_open_fd(&c, fd) != 0) {
        snprintf(info->err, sizeof(info->err), "%s", lup_client_error(&c));
        return LOCI_PROBE_NO_LINK;
    }
    return check_id(&c, info);
#else
    (void)fd;
    return LOCI_PROBE_NO_BACKEND;
#endif
}

loci_probe_result_t loci_hw_probe(const char* dev, loci_probe_info_t* info) {
    memset(info, 0, sizeof(*info));
#ifdef LOCI_BE_HAS_HW
    if ((info->busy_pid = loci_hw_port_user(dev)) != 0) return LOCI_PROBE_BUSY;
    lup_client_t c;
    if (lup_open(&c, dev) != 0) {
        snprintf(info->err, sizeof(info->err), "%s", lup_client_error(&c));
        return LOCI_PROBE_NO_LINK;
    }
    loci_probe_result_t r = check_id(&c, info);
    lup_close(&c);
    return r;
#else
    (void)dev;
    return LOCI_PROBE_NO_BACKEND;
#endif
}
