/**
 * @file netutil.c
 * @brief Utilitaires réseau — implémentation.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extrait de main.c (Epic 7 / US1, Sprint 125), à l'identique.
 */
#include "utils/netutil.h"

#include <string.h>
#include <stdlib.h>

void parse_host_port(const char* spec, char* host, size_t host_sz,
                     uint16_t* port, uint16_t def_port)
{
    *port = def_port;
    host[0] = '\0';
    const char* colon = strrchr(spec, ':');
    if (colon && colon != spec) {
        size_t hlen = (size_t)(colon - spec);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host, spec, hlen);
        host[hlen] = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host, spec, host_sz - 1);
        host[host_sz - 1] = '\0';
    }
}
