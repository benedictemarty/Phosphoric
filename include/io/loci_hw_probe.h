/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_hw_probe.h
 * @brief Reconnaissance d'une LOCI-USB : matériel, protocole loci-usb et
 *        firmware LOCI (marqueur 'L' en $0319)
 * @author bmarty <bmarty@mailo.com>
 *
 * Trois étages, chacun nécessaire : le nom de produit USB (loci_usb_detect,
 * picowifi_detect.h) ne dit que « une carte LOCI-USB est branchée » ; le PING
 * du protocole loci-usb prouve que le pont répond ; la lecture de $0319 = 'L'
 * (LOCI_DSK_IO_ID, loci.h) prouve que le firmware LOCI tourne derrière. La
 * lecture de $0319 n'a pas d'effet de bord (registre d'identité, lecture seule).
 */
#ifndef LOCI_HW_PROBE_H
#define LOCI_HW_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LOCI_PROBE_OK = 0,
    LOCI_PROBE_NO_BACKEND,   /* binaire sans backend LOCI-USB (LOCI_HW=0, Windows, web) */
    LOCI_PROBE_BUSY,         /* port ouvert par un autre programme */
    LOCI_PROBE_NO_LINK,      /* ouverture ou PING loci-usb en échec */
    LOCI_PROBE_NOT_LOCI      /* le pont répond mais $0319 n'est pas 'L' */
} loci_probe_result_t;

typedef struct {
    uint8_t proto, fw, caps;   /* PING : version du protocole, du firmware du pont */
    uint8_t id;                /* octet lu en $0319 */
    int     busy_pid;          /* LOCI_PROBE_BUSY : programme qui tient le port */
    char    err[128];          /* LOCI_PROBE_NO_LINK : cause */
} loci_probe_info_t;

/* Vérifie la LOCI-USB sur @p dev (ex. /dev/ttyACM0) ; ferme le port ensuite. */
loci_probe_result_t loci_hw_probe(const char* dev, loci_probe_info_t* info);
/* Idem sur un descripteur déjà ouvert (tests, PTY) ; ne le ferme pas. */
loci_probe_result_t loci_hw_probe_fd(int fd, loci_probe_info_t* info);
/* PID d'un autre processus qui a @p dev ouvert (/proc/<pid>/fd), 0 sinon. */
int loci_hw_port_user(const char* dev);

#endif /* LOCI_HW_PROBE_H */
