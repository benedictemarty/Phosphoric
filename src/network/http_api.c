/**
 * @file http_api.c
 * @brief Optional HTTP control API server (sprint 94, API REST Epic 3).
 *
 * See http_api.h. A single background thread accepts one connection at a time,
 * parses a small subset of HTTP/1.1, maps the request to a `--control` command
 * line, runs it on the emulator thread via control_queue_submit() (which blocks
 * until the main loop drains the queue at a frame boundary), and returns the
 * reply as JSON. All command logic lives in control_dispatch(); this file is
 * purely transport + routing + a path sandbox.
 */

#ifdef HAS_HTTPAPI

#define _GNU_SOURCE   /* strcasestr, MSG_NOSIGNAL */
#include "network/http_api.h"
#include "control_queue.h"
#include "utils/logging.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_REQ_MAX   8192
#define HTTP_ROOT_MAX  1024
#define HTTP_CMD_MAX   2048

struct http_api_server_s {
    emulator_t*     emu;
    control_queue_t* queue;
    int             listen_fd;
    pthread_t       thread;
    volatile bool   running;
    char            root[HTTP_ROOT_MAX];
};

/* ─── small helpers ────────────────────────────────────────────────── */

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void send_all(int fd, const char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        off += (size_t)n;
    }
}

/* In-place URL-decode (%XX and '+' → space). */
static void url_decode(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            int hi = r[1], lo = r[2];
            #define HEXV(c) ((c) <= '9' ? (c) - '0' : ((c) | 0x20) - 'a' + 10)
            *w++ = (char)((HEXV(hi) << 4) | HEXV(lo));
            #undef HEXV
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* Extract form/query parameter @p key from @p qs ("a=1&b=2") into @p out
 * (url-decoded). Returns true if found. */
static bool get_param(const char* qs, const char* key, char* out, size_t outsz) {
    if (!qs) return false;
    size_t klen = strlen(key);
    for (const char* p = qs; p && *p; ) {
        const char* eq = strchr(p, '=');
        const char* amp = strchr(p, '&');
        if (eq && (!amp || eq < amp) && (size_t)(eq - p) == klen &&
            strncmp(p, key, klen) == 0) {
            const char* vs = eq + 1;
            size_t vlen = amp ? (size_t)(amp - vs) : strlen(vs);
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, vs, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

/* Resolve a user-supplied relative path inside the sandbox root. Rejects
 * absolute paths and any ".." component. Returns true on success. */
static bool sandbox_path(const http_api_server_t* srv, const char* rel,
                         char* out, size_t outsz) {
    if (!rel || !*rel) return false;
    if (rel[0] == '/') return false;                 /* no absolute paths */
    if (strstr(rel, "..")) return false;             /* no parent escape  */
    int n = snprintf(out, outsz, "%s/%s", srv->root, rel);
    return n > 0 && (size_t)n < outsz;
}

/* JSON-escape @p in into @p out (handles ", \\, control chars, newline). */
static void json_escape(const char* in, char* out, size_t outsz) {
    size_t o = 0;
    for (const char* p = in; *p && o + 7 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)  { o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* ─── HTTP responses ───────────────────────────────────────────────── */

static void http_send(int fd, int status, const char* status_text,
                      const char* body) {
    char hdr[256];
    size_t blen = strlen(body);
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, blen);
    if (hlen > 0) send_all(fd, hdr, (size_t)hlen);
    send_all(fd, body, blen);
}

/* Translate a control reply line ("OK …" / "ERR …") into a JSON response. */
static void respond_reply(int fd, const char* reply) {
    char stripped[HTTP_CMD_MAX];
    snprintf(stripped, sizeof(stripped), "%s", reply ? reply : "");
    size_t sl = strlen(stripped);
    while (sl && (stripped[sl - 1] == '\n' || stripped[sl - 1] == '\r'))
        stripped[--sl] = '\0';

    bool ok = (strncmp(stripped, "OK", 2) == 0) &&
              (stripped[2] == '\0' || stripped[2] == ' ');
    const char* payload = stripped;
    if (ok) payload += (stripped[2] == ' ') ? 3 : 2;          /* skip "OK " */
    else if (strncmp(stripped, "ERR ", 4) == 0) payload += 4; /* skip "ERR " */

    char esc[HTTP_CMD_MAX * 2];
    json_escape(payload, esc, sizeof(esc));
    char body[HTTP_CMD_MAX * 2 + 64];
    if (ok)
        snprintf(body, sizeof(body), "{\"ok\":true,\"reply\":\"%s\"}\n", esc);
    else
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}\n", esc);
    http_send(fd, ok ? 200 : 400, ok ? "OK" : "Bad Request", body);
}

/* ─── routing: HTTP request → control command line ─────────────────── */

/* Build the control command for @p method @p path (@p query, @p body) into
 * @p cmd. On success returns true. On a routing/validation failure returns
 * false and writes a JSON error directly to @p fd (caller then just closes). */
static bool route(http_api_server_t* srv, int fd, const char* method,
                  char* path, const char* query, const char* body,
                  char* cmd, size_t cmdsz) {
    char arg[HTTP_ROOT_MAX];

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0) {
            http_send(fd, 200, "OK",
                "{\"ok\":true,\"reply\":\"phosphoric http-api; "
                "GET /hello /regs /mem?addr=&len=[&bank=cpu|ram|rom|overlay] /peek/{via|psg|disk|acia|tape|loci|video|kbd|joy|printer} "
                "/break /watch /disasm?addr=&n= /trace /watch-region; "
                "POST /reset /mem /keys /tape /disk/{A-D} /exec/{step|next|step-out|continue|pause} "
                "/break(addr,if) /watch(addr,mode) /raster(line) /set(reg|via,val) "
                "/hunt(op,val) /sym(path) /save(path,addr,len) /load(path,addr) "
                "/state/save /state/load /trace(spec) /trace/stop /trace/save(path) "
                "/watch-region(start,end,flags); "
                "DELETE /tape /disk/{A-D} /break/{id} /watch/{id} /raster/{id}\"}\n");
            return false;
        }
        if (strcmp(path, "/hello") == 0) { snprintf(cmd, cmdsz, "hello"); return true; }
        if (strcmp(path, "/regs") == 0)  { snprintf(cmd, cmdsz, "regs");  return true; }
        if (strcmp(path, "/mem") == 0) {
            char addr[32], len[32], bank[16];
            if (!get_param(query, "addr", addr, sizeof(addr)) ||
                !get_param(query, "len", len, sizeof(len))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"/mem needs ?addr=&len= [&bank=cpu|ram|rom|overlay]\"}\n");
                return false;
            }
            if (get_param(query, "bank", bank, sizeof(bank)))
                snprintf(cmd, cmdsz, "read %s %s %s", addr, len, bank);
            else
                snprintf(cmd, cmdsz, "read %s %s", addr, len);
            return true;
        }
        if (strncmp(path, "/peek/", 6) == 0) {
            snprintf(cmd, cmdsz, "peek %s", path + 6);
            return true;
        }
        if (strcmp(path, "/break") == 0) { snprintf(cmd, cmdsz, "break-list"); return true; }
        if (strcmp(path, "/watch") == 0) { snprintf(cmd, cmdsz, "watch-list"); return true; }
        if (strcmp(path, "/disasm") == 0) {
            char addr[32], n[32];
            if (!get_param(query, "addr", addr, sizeof(addr)) ||
                !get_param(query, "n", n, sizeof(n))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"/disasm needs ?addr=&n=\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "disasm %s %s", addr, n);
            return true;
        }
        if (strcmp(path, "/trace") == 0) { snprintf(cmd, cmdsz, "trace status"); return true; }
        if (strcmp(path, "/watch-region") == 0) { snprintf(cmd, cmdsz, "watch-region-list"); return true; }
    }
    else if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/reset") == 0) { snprintf(cmd, cmdsz, "reset"); return true; }
        if (strcmp(path, "/keys") == 0) {
            char text[HTTP_CMD_MAX - 16];
            if (!get_param(body, "text", text, sizeof(text))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /keys needs text= "
                    "(\\\\n = RETURN)\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "keys %s", text);
            return true;
        }
        if (strncmp(path, "/exec/", 6) == 0) {
            const char* w = path + 6;
            if (strcmp(w, "step") && strcmp(w, "next") && strcmp(w, "step-out") &&
                strcmp(w, "continue") && strcmp(w, "pause")) {
                http_send(fd, 404, "Not Found",
                    "{\"ok\":false,\"error\":\"unknown exec verb\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "%s", w);
            return true;
        }
        if (strcmp(path, "/mem") == 0) {
            char addr[32], bytes[HTTP_CMD_MAX - 64];
            if (!get_param(body, "addr", addr, sizeof(addr)) ||
                !get_param(body, "bytes", bytes, sizeof(bytes))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /mem needs addr= & bytes=\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "write %s %s", addr, bytes);
            return true;
        }
        if (strcmp(path, "/tape") == 0) {
            if (!get_param(body, "path", arg, sizeof(arg))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /tape needs path=\"}\n");
                return false;
            }
            char full[HTTP_ROOT_MAX];
            if (!sandbox_path(srv, arg, full, sizeof(full))) {
                http_send(fd, 403, "Forbidden",
                    "{\"ok\":false,\"error\":\"path outside sandbox root\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "load-tap %s", full);
            return true;
        }
        if (strncmp(path, "/disk/", 6) == 0 && path[6] && !path[7]) {
            if (!get_param(body, "path", arg, sizeof(arg))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /disk needs path=\"}\n");
                return false;
            }
            char full[HTTP_ROOT_MAX];
            if (!sandbox_path(srv, arg, full, sizeof(full))) {
                http_send(fd, 403, "Forbidden",
                    "{\"ok\":false,\"error\":\"path outside sandbox root\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "load-disk %c %s", path[6], full);
            return true;
        }
        if (strcmp(path, "/break") == 0) {
            char addr[32], cond[160];
            if (!get_param(body, "addr", addr, sizeof(addr))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /break needs addr= [&if=EXPR]\"}\n");
                return false;
            }
            if (get_param(body, "if", cond, sizeof(cond)))
                snprintf(cmd, cmdsz, "break %s if %s", addr, cond);
            else
                snprintf(cmd, cmdsz, "break %s", addr);
            return true;
        }
        if (strcmp(path, "/watch") == 0) {
            char addr[32], mode[8];
            if (!get_param(body, "addr", addr, sizeof(addr))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /watch needs addr= [&mode=w|r|a|c]\"}\n");
                return false;
            }
            if (get_param(body, "mode", mode, sizeof(mode)))
                snprintf(cmd, cmdsz, "watch %s %s", addr, mode);
            else
                snprintf(cmd, cmdsz, "watch %s", addr);
            return true;
        }
        if (strcmp(path, "/raster") == 0) {
            char line[32];
            if (!get_param(body, "line", line, sizeof(line))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /raster needs line=\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "raster %s", line);
            return true;
        }
        if (strcmp(path, "/set") == 0) {
            char reg[16], val[32], via[16];
            if (get_param(body, "via", via, sizeof(via)) &&
                get_param(body, "val", val, sizeof(val))) {
                snprintf(cmd, cmdsz, "set via %s %s", via, val);
                return true;
            }
            if (get_param(body, "reg", reg, sizeof(reg)) &&
                get_param(body, "val", val, sizeof(val))) {
                snprintf(cmd, cmdsz, "set %s %s", reg, val);
                return true;
            }
            http_send(fd, 400, "Bad Request",
                "{\"ok\":false,\"error\":\"POST /set needs reg=&val= (or via=&val=)\"}\n");
            return false;
        }
        if (strcmp(path, "/hunt") == 0) {
            char op[16], val[32];
            if (!get_param(body, "op", op, sizeof(op))) {
                snprintf(cmd, cmdsz, "hunt");   /* seed */
                return true;
            }
            if (get_param(body, "val", val, sizeof(val)))
                snprintf(cmd, cmdsz, "hunt %s %s", op, val);
            else
                snprintf(cmd, cmdsz, "hunt %s", op);
            return true;
        }
        if (strcmp(path, "/sym") == 0) {
            if (!get_param(body, "path", arg, sizeof(arg))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /sym needs path=\"}\n");
                return false;
            }
            char full[HTTP_ROOT_MAX];
            if (!sandbox_path(srv, arg, full, sizeof(full))) {
                http_send(fd, 403, "Forbidden",
                    "{\"ok\":false,\"error\":\"path outside sandbox root\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "load-sym %s", full);
            return true;
        }
        if (strcmp(path, "/save") == 0) {
            char addr[32], len[32];
            if (!get_param(body, "path", arg, sizeof(arg)) ||
                !get_param(body, "addr", addr, sizeof(addr)) ||
                !get_param(body, "len", len, sizeof(len))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /save needs path=&addr=&len=\"}\n");
                return false;
            }
            char full[HTTP_ROOT_MAX];
            if (!sandbox_path(srv, arg, full, sizeof(full))) {
                http_send(fd, 403, "Forbidden",
                    "{\"ok\":false,\"error\":\"path outside sandbox root\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "save-mem %s %s %s", full, addr, len);
            return true;
        }
        if (strcmp(path, "/load") == 0) {
            char addr[32];
            if (!get_param(body, "path", arg, sizeof(arg)) ||
                !get_param(body, "addr", addr, sizeof(addr))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /load needs path=&addr=\"}\n");
                return false;
            }
            char full[HTTP_ROOT_MAX];
            if (!sandbox_path(srv, arg, full, sizeof(full))) {
                http_send(fd, 403, "Forbidden",
                    "{\"ok\":false,\"error\":\"path outside sandbox root\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "load-mem %s %s", full, addr);
            return true;
        }
        if (strcmp(path, "/trace") == 0) {
            char spec[192];
            if (get_param(body, "spec", spec, sizeof(spec)))
                snprintf(cmd, cmdsz, "trace start %s", spec);
            else
                snprintf(cmd, cmdsz, "trace start now");
            return true;
        }
        if (strcmp(path, "/watch-region") == 0) {
            char start[32], end[32], flags[8];
            if (!get_param(body, "start", start, sizeof(start)) ||
                !get_param(body, "end", end, sizeof(end))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /watch-region needs start=&end= [&flags=rwx]\"}\n");
                return false;
            }
            if (get_param(body, "flags", flags, sizeof(flags)))
                snprintf(cmd, cmdsz, "watch-region %s %s %s", start, end, flags);
            else
                snprintf(cmd, cmdsz, "watch-region %s %s", start, end);
            return true;
        }
        if (strcmp(path, "/trace/stop") == 0) { snprintf(cmd, cmdsz, "trace stop"); return true; }
        if (strcmp(path, "/trace/save") == 0) {
            if (!get_param(body, "path", arg, sizeof(arg))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"POST /trace/save needs path=\"}\n");
                return false;
            }
            char full[HTTP_ROOT_MAX];
            if (!sandbox_path(srv, arg, full, sizeof(full))) {
                http_send(fd, 403, "Forbidden",
                    "{\"ok\":false,\"error\":\"path outside sandbox root\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "trace save %s", full);
            return true;
        }
        if (strcmp(path, "/state/save") == 0 || strcmp(path, "/state/load") == 0) {
            if (!get_param(body, "path", arg, sizeof(arg))) {
                http_send(fd, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"needs path=\"}\n");
                return false;
            }
            char full[HTTP_ROOT_MAX];
            if (!sandbox_path(srv, arg, full, sizeof(full))) {
                http_send(fd, 403, "Forbidden",
                    "{\"ok\":false,\"error\":\"path outside sandbox root\"}\n");
                return false;
            }
            snprintf(cmd, cmdsz, "%s %s",
                     strcmp(path, "/state/save") == 0 ? "state-save" : "state-load", full);
            return true;
        }
    }
    else if (strcmp(method, "DELETE") == 0) {
        if (strcmp(path, "/tape") == 0) { snprintf(cmd, cmdsz, "eject-tape"); return true; }
        if (strncmp(path, "/disk/", 6) == 0 && path[6] && !path[7]) {
            snprintf(cmd, cmdsz, "eject-disk %c", path[6]);
            return true;
        }
        if (strncmp(path, "/break/", 7) == 0 && path[7]) {
            snprintf(cmd, cmdsz, "unbreak %.15s", path + 7); return true;
        }
        if (strncmp(path, "/watch/", 7) == 0 && path[7]) {
            snprintf(cmd, cmdsz, "unwatch %.15s", path + 7); return true;
        }
        if (strncmp(path, "/raster/", 8) == 0 && path[8]) {
            snprintf(cmd, cmdsz, "unraster %.15s", path + 8); return true;
        }
        if (strcmp(path, "/trace") == 0) { snprintf(cmd, cmdsz, "trace off"); return true; }
        if (strcmp(path, "/watch-region") == 0) { snprintf(cmd, cmdsz, "watch-region-clear"); return true; }
    }

    http_send(fd, 404, "Not Found",
        "{\"ok\":false,\"error\":\"unknown route\"}\n");
    return false;
}

/* ─── per-connection handling ──────────────────────────────────────── */

static void handle_client(http_api_server_t* srv, int fd) {
    char req[HTTP_REQ_MAX];
    size_t total = 0;
    ssize_t n;
    /* Read until we have the header terminator, then satisfy Content-Length. */
    size_t header_end = 0, content_len = 0, body_start = 0;
    for (;;) {
        n = recv(fd, req + total, sizeof(req) - 1 - total, 0);
        if (n <= 0) { if (total == 0) return; break; }
        total += (size_t)n;
        req[total] = '\0';
        if (!header_end) {
            char* h = strstr(req, "\r\n\r\n");
            if (h) {
                header_end = (size_t)(h - req);
                body_start = header_end + 4;
                const char* cl = strcasestr(req, "Content-Length:");
                if (cl) content_len = (size_t)strtoul(cl + 15, NULL, 10);
            }
        }
        if (header_end && total >= body_start + content_len) break;
        if (total >= sizeof(req) - 1) break;
    }
    if (!header_end) return;

    /* Parse the request line: METHOD SP PATH SP HTTP/x. */
    char method[16] = {0}, target[2048] = {0};
    if (sscanf(req, "%15s %2047s", method, target) != 2) {
        http_send(fd, 400, "Bad Request",
            "{\"ok\":false,\"error\":\"malformed request line\"}\n");
        return;
    }
    char* query = strchr(target, '?');
    if (query) { *query = '\0'; query++; }

    /* NUL-terminate the body at Content-Length. */
    char* body = req + body_start;
    if (body_start + content_len < sizeof(req)) body[content_len] = '\0';

    char cmd[HTTP_CMD_MAX];
    if (!route(srv, fd, method, target, query, body, cmd, sizeof(cmd)))
        return;   /* route() already answered (help/error) */

    /* Execute on the emulator thread via the queue; blocks until drained. */
    char* reply = NULL;
    control_queue_submit(srv->queue, cmd, &reply, NULL);
    respond_reply(fd, reply ? reply : "ERR no reply");
    free(reply);
}

static void* server_thread(void* arg) {
    http_api_server_t* srv = (http_api_server_t*)arg;
    while (srv->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv->listen_fd, &rfds);
        struct timeval tv = {0, 100000};   /* 100 ms */
        int r = select(srv->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;
        if (!FD_ISSET(srv->listen_fd, &rfds)) continue;
        int cfd = accept(srv->listen_fd, NULL, NULL);
        if (cfd < 0) continue;
        handle_client(srv, cfd);
        close(cfd);
    }
    return NULL;
}

/* ─── public API ───────────────────────────────────────────────────── */

http_api_server_t* http_api_start(emulator_t* emu, control_queue_t* queue,
                                  uint16_t port, const char* bind_addr,
                                  const char* root) {
    if (!queue) return NULL;
    if (port == 0) port = HTTP_API_DEFAULT_PORT;
    if (!bind_addr || !*bind_addr) bind_addr = HTTP_API_DEFAULT_BIND;
    if (!root || !*root) root = ".";

    http_api_server_t* srv = (http_api_server_t*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    srv->emu = emu;
    srv->queue = queue;
    snprintf(srv->root, sizeof(srv->root), "%s", root);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) { log_error("http-api: socket() failed"); free(srv); return NULL; }
    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        log_error("http-api: invalid bind address '%s'", bind_addr);
        close(srv->listen_fd); free(srv); return NULL;
    }
    if (bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        log_error("http-api: bind %s:%u failed (%s)", bind_addr, port, strerror(errno));
        close(srv->listen_fd); free(srv); return NULL;
    }
    if (listen(srv->listen_fd, 4) != 0) {
        log_error("http-api: listen failed (%s)", strerror(errno));
        close(srv->listen_fd); free(srv); return NULL;
    }
    set_nonblocking(srv->listen_fd);

    srv->running = true;
    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
        log_error("http-api: pthread_create failed");
        close(srv->listen_fd); free(srv); return NULL;
    }
    log_info("http-api: listening on %s:%u (sandbox root '%s')", bind_addr, port, root);
    return srv;
}

void http_api_stop(http_api_server_t* server) {
    if (!server) return;
    server->running = false;
    pthread_join(server->thread, NULL);
    close(server->listen_fd);
    free(server);
}

#else
typedef int http_api_translation_unit_not_empty;
#endif /* HAS_HTTPAPI */
