/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file disk_http.h
 * @brief Minimal blocking HTTP client for the web-backed disk (loci-webdisk).
 * @author bmarty <bmarty@mailo.com>
 *
 * Fetches a byte range of a remote disk image from the Python disk_server.py
 * used by the loci-webdisk project (architecture B): the LOCI/Microdisc FDC
 * pulls raw MFM tracks on demand instead of reading a local .dsk. The range is
 * carried in the query-string (?offset=&len=), matching the server API — no
 * HTTP Range header, no TLS (plain http:// only). Standalone (no libcurl).
 */
#ifndef STORAGE_DISK_HTTP_H
#define STORAGE_DISK_HTTP_H

#include <stdint.h>

/**
 * GET <base_url>?offset=<offset>&len=<len> and copy the raw response body into
 * @p out (up to @p outcap bytes). Accepts HTTP status 200 and 206.
 *
 * @return number of body bytes written to @p out, or -1 on any failure
 *         (bad URL, connection refused, non-2xx status, short read).
 */
long disk_http_get(const char* base_url, long offset, long len,
                   uint8_t* out, long outcap);

#endif /* STORAGE_DISK_HTTP_H */
