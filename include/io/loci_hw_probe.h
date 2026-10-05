/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_hw_probe.h
 * @brief Recognising a LOCI-USB: hardware, loci-usb protocol and LOCI
 *        firmware ('L' marker at $0319)
 * @author bmarty <bmarty@mailo.com>
 *
 * Three stages, each one needed: the USB product name (loci_usb_detect,
 * picowifi_detect.h) only says « a LOCI-USB board is plugged in »; the loci-usb
 * protocol PING proves the bridge answers; reading $0319 = 'L' (LOCI_DSK_IO_ID,
 * loci.h) proves the LOCI firmware runs behind it. Reading $0319 has no side
 * effect (identity register, read-only).
 */
#ifndef LOCI_HW_PROBE_H
#define LOCI_HW_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LOCI_PROBE_OK = 0,
    LOCI_PROBE_NO_BACKEND,   /* binary without the LOCI-USB backend (LOCI_HW=0, Windows, web) */
    LOCI_PROBE_BUSY,         /* port opened by another program */
    LOCI_PROBE_NO_LINK,      /* open or loci-usb PING failed */
    LOCI_PROBE_NOT_LOCI      /* the bridge answers but $0319 is not 'L' */
} loci_probe_result_t;

typedef struct {
    uint8_t proto, fw, caps;   /* PING: protocol version, bridge firmware version */
    uint8_t id;                /* byte read at $0319 */
    int     busy_pid;          /* LOCI_PROBE_BUSY: program holding the port */
    char    err[128];          /* LOCI_PROBE_NO_LINK: cause */
} loci_probe_info_t;

/* Checks the LOCI-USB on @p dev (e.g. /dev/ttyACM0); closes the port afterwards. */
loci_probe_result_t loci_hw_probe(const char* dev, loci_probe_info_t* info);
/* Same on an already open descriptor (tests, PTY); does not close it. */
loci_probe_result_t loci_hw_probe_fd(int fd, loci_probe_info_t* info);
/* PID of another process that has @p dev open (/proc/<pid>/fd), 0 otherwise. */
int loci_hw_port_user(const char* dev);

#endif /* LOCI_HW_PROBE_H */
