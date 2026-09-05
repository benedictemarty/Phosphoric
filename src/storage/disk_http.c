/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file disk_http.c
 * @brief Minimal blocking HTTP client for the web-backed disk (loci-webdisk).
 * @author bmarty <bmarty@mailo.com>
 */
#define _GNU_SOURCE
#include "storage/disk_http.h"
#include "utils/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

/* Split "http://host[:port]/path" → host, port, path (no leading slash). */
static int parse_url(const char* url, char* host, size_t hsz,
                     char* port, size_t psz, char* path, size_t pthsz) {
    const char* p = url;
    if (strncasecmp(p, "http://", 7) != 0) return -1;   /* http only, no TLS */
    p += 7;
    snprintf(port, psz, "%s", "80");
    const char* slash = strchr(p, '/');
    const char* hostend = slash ? slash : (p + strlen(p));
    const char* colon = memchr(p, ':', (size_t)(hostend - p));
    size_t hl = (size_t)((colon ? colon : hostend) - p);
    if (hl >= hsz) hl = hsz - 1;
    memcpy(host, p, hl); host[hl] = '\0';
    if (colon) {
        size_t pl = (size_t)(hostend - (colon + 1));
        if (pl >= psz) pl = psz - 1;
        memcpy(port, colon + 1, pl); port[pl] = '\0';
    }
    if (slash) snprintf(path, pthsz, "%s", slash + 1);
    else       path[0] = '\0';
    return 0;
}

static int tcp_connect_blocking(const char* host, const char* port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Read exactly n bytes (blocking) unless the peer closes early. */
static long read_full(int fd, uint8_t* buf, long n) {
    long got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, (size_t)(n - got));
        if (r > 0) { got += r; continue; }
        if (r == 0) break;                 /* peer closed */
        return got;                        /* error */
    }
    return got;
}

/* Read one CRLF-terminated line (CR dropped). Returns length; 0 = blank line. */
static int read_line(int fd, char* buf, size_t sz) {
    size_t n = 0;
    while (n < sz - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r != 1) break;
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = '\0';
    return (int)n;
}

long disk_http_get(const char* base_url, long offset, long len,
                   uint8_t* out, long outcap) {
    char host[256], port[16], path[400];
    if (parse_url(base_url, host, sizeof(host), port, sizeof(port),
                  path, sizeof(path)) != 0) {
        log_error("disk_http: not an http:// URL: %s", base_url);
        return -1;
    }
    int fd = tcp_connect_blocking(host, port);
    if (fd < 0) { log_error("disk_http: connect %s:%s failed", host, port); return -1; }

    /* The server carries the byte range in the query-string (?offset=&len=). */
    char sep = strchr(path, '?') ? '&' : '?';
    char req[700];
    int rn = snprintf(req, sizeof(req),
                      "GET /%s%coffset=%ld&len=%ld HTTP/1.1\r\nHost: %s\r\n"
                      "Connection: close\r\n\r\n",
                      path, sep, offset, len, host);
    if (write(fd, req, (size_t)rn) != rn) { close(fd); return -1; }

    /* Status line + headers → status code and Content-Length. */
    char line[256];
    int status = 0;
    long clen = -1;
    int n = read_line(fd, line, sizeof(line));
    if (n > 0) { char* sp = strchr(line, ' '); if (sp) status = atoi(sp + 1); }
    for (;;) {
        n = read_line(fd, line, sizeof(line));
        if (n <= 0) break;                                  /* blank line/EOF */
        if (!strncasecmp(line, "Content-Length:", 15)) clen = atol(line + 15);
    }
    if (status != 200 && status != 206) {
        log_error("disk_http: %s?offset=%ld&len=%ld → HTTP %d", base_url, offset, len, status);
        close(fd); return -1;
    }
    long want = (clen >= 0 && clen < outcap) ? clen : outcap;
    long got = read_full(fd, out, want);
    close(fd);
    return got;
}
