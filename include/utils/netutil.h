/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file netutil.h
 * @brief Network utilities -- parsing of host:port addresses.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extracted from main.c (Epic 7 / US1, Sprint 125): pure function, with no state
 * and no dependency on the emulator core.
 */
#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Parses a "host[:port]" specification.
 *
 * If a ':' is present (and not at the start), the left part is the host and the
 * right part the port; otherwise the whole string is the host and `def_port` is
 * used. `host` is always NUL-terminated and truncated to `host_sz`.
 *
 * @param spec      Input string (e.g. "127.0.0.1:6551" or "localhost").
 * @param host      Output buffer for the host.
 * @param host_sz   Size of the host buffer.
 * @param port      Output: parsed port (or def_port).
 * @param def_port  Default port if absent.
 */
void parse_host_port(const char* spec, char* host, size_t host_sz,
                     uint16_t* port, uint16_t def_port);

#endif /* NETUTIL_H */
