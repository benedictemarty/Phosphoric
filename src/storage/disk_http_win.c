/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file disk_http_win.c
 * @brief Web-backed disk HTTP client — Windows (MinGW-w64) placeholder.
 * @author bmarty <bmarty@mailo.com>
 *
 * disk_http.c is a minimal blocking HTTP client built on POSIX sockets
 * (<sys/socket.h>/<netdb.h>). The Windows v1 build links this placeholder
 * instead so --disk-web / --loci-web fail with a clear message rather than a
 * build error. Porting to Winsock is tracked as follow-up work (same scope as
 * the other Linux-only network transports).
 */

#include "storage/disk_http.h"
#include "utils/logging.h"

long disk_http_get(const char* base_url, long offset, long len,
                   uint8_t* out, long outcap)
{
    (void)base_url; (void)offset; (void)len; (void)out; (void)outcap;
    log_error("disque web HTTP (--disk-web / --loci-web) : non disponible "
              "dans le build Windows v1 (client HTTP sur sockets POSIX) — "
              "utilisez la version Linux/WSL2");
    return -1;
}
