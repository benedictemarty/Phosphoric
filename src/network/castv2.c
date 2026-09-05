/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file castv2.c
 * @brief Native Google Cast V2 protocol client
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-28
 * @version 1.3.0-alpha
 *
 * Implements the CASTV2 protocol to control Chromecast devices natively:
 * - Hand-coded protobuf for CastMessage (no library dependency)
 * - TLS connection via OpenSSL (self-signed cert accepted)
 * - Cast protocol: CONNECT, LAUNCH DashCast, parse RECEIVER_STATUS, send URL
 * - Heartbeat thread (PING every 5s)
 * - mDNS discovery wrapper
 * - Local IP detection
 *
 * Build with CAST=1 to enable (adds -DHAS_CAST -lpthread -lssl -lcrypto).
 */

#ifdef HAS_CAST

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "network/cast_server.h"
#include "utils/logging.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  PROTOBUF ENCODING (hand-coded, no library)                        */
/* ═══════════════════════════════════════════════════════════════════ */

/*
 * CastMessage protobuf fields:
 *   Field 1: protocol_version (varint, always 0 = CASTV2_1_0)
 *   Field 2: source_id (string, "sender-0")
 *   Field 3: destination_id (string, "receiver-0" or transportId)
 *   Field 4: namespace (string, URN)
 *   Field 5: payload_type (varint, 0 = STRING)
 *   Field 6: payload_utf8 (string, JSON)
 *
 * Protobuf wire format:
 *   varint field: (field_num << 3 | 0) + varint value
 *   string field: (field_num << 3 | 2) + varint length + bytes
 *
 * Framing: [4 bytes big-endian length][protobuf CastMessage]
 */

int castv2_encode_varint(uint8_t* buf, int buf_size, uint64_t value) {
    int pos = 0;
    do {
        if (pos >= buf_size) return -1;
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value > 0) byte |= 0x80;
        buf[pos++] = byte;
    } while (value > 0);
    return pos;
}

int castv2_decode_varint(const uint8_t* buf, int buf_size, uint64_t* value) {
    *value = 0;
    int shift = 0;
    int pos = 0;
    while (pos < buf_size && shift < 64) {
        uint8_t byte = buf[pos++];
        *value |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return pos;
        shift += 7;
    }
    return -1;
}

/* Encode a protobuf varint field: (field_num << 3 | wire_type=0) + varint value */
static int pb_encode_varint_field(uint8_t* buf, int buf_size, int field_num, uint64_t value) {
    if (buf_size < 2) return -1;
    int pos = 0;
    /* Tag: field_num << 3 | 0 (varint wire type) */
    int n = castv2_encode_varint(buf + pos, buf_size - pos, (uint64_t)((field_num << 3) | 0));
    if (n < 0) return -1;
    pos += n;
    n = castv2_encode_varint(buf + pos, buf_size - pos, value);
    if (n < 0) return -1;
    pos += n;
    return pos;
}

/* Encode a protobuf string field: tag + length + bytes */
static int pb_encode_string(uint8_t* buf, int buf_size, int field_num, const char* str) {
    int slen = (int)strlen(str);
    if (buf_size < 2 + slen) return -1;
    int pos = 0;
    /* Tag: field_num << 3 | 2 (length-delimited wire type) */
    int n = castv2_encode_varint(buf + pos, buf_size - pos, (uint64_t)((field_num << 3) | 2));
    if (n < 0) return -1;
    pos += n;
    /* Length */
    n = castv2_encode_varint(buf + pos, buf_size - pos, (uint64_t)slen);
    if (n < 0) return -1;
    pos += n;
    /* String data */
    if (pos + slen > buf_size) return -1;
    memcpy(buf + pos, str, (size_t)slen);
    pos += slen;
    return pos;
}

int castv2_build_message(uint8_t* buf, int buf_size,
                         const char* source_id, const char* dest_id,
                         const char* ns, const char* payload) {
    /* Reserve 4 bytes for framing header */
    int pos = 4;
    int n;

    /* Field 1: protocol_version = 0 (CASTV2_1_0) */
    n = pb_encode_varint_field(buf + pos, buf_size - pos, 1, 0);
    if (n < 0) return -1;
    pos += n;

    /* Field 2: source_id */
    n = pb_encode_string(buf + pos, buf_size - pos, 2, source_id);
    if (n < 0) return -1;
    pos += n;

    /* Field 3: destination_id */
    n = pb_encode_string(buf + pos, buf_size - pos, 3, dest_id);
    if (n < 0) return -1;
    pos += n;

    /* Field 4: namespace */
    n = pb_encode_string(buf + pos, buf_size - pos, 4, ns);
    if (n < 0) return -1;
    pos += n;

    /* Field 5: payload_type = 0 (STRING) */
    n = pb_encode_varint_field(buf + pos, buf_size - pos, 5, 0);
    if (n < 0) return -1;
    pos += n;

    /* Field 6: payload_utf8 (JSON string) */
    n = pb_encode_string(buf + pos, buf_size - pos, 6, payload);
    if (n < 0) return -1;
    pos += n;

    /* Write 4-byte big-endian length prefix (message length without the 4 header bytes) */
    uint32_t msg_len = (uint32_t)(pos - 4);
    buf[0] = (uint8_t)((msg_len >> 24) & 0xFF);
    buf[1] = (uint8_t)((msg_len >> 16) & 0xFF);
    buf[2] = (uint8_t)((msg_len >> 8) & 0xFF);
    buf[3] = (uint8_t)(msg_len & 0xFF);

    return pos;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TLS CONNECTION                                                     */
/* ═══════════════════════════════════════════════════════════════════ */

static bool castv2_tls_connect(castv2_client_t* client, const char* ip) {
    /* Create TCP socket */
    client->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sock_fd < 0) {
        log_error("CastV2: socket() failed: %s", strerror(errno));
        return false;
    }

    /* Set 2s recv timeout, 5s send timeout */
    struct timeval tv_recv = { .tv_sec = 2, .tv_usec = 0 };
    struct timeval tv_send = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));
    setsockopt(client->sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));

    /* Connect TCP */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CASTV2_PORT);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        log_error("CastV2: invalid IP: %s", ip);
        close(client->sock_fd);
        client->sock_fd = -1;
        return false;
    }

    if (connect(client->sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("CastV2: connect() to %s:%d failed: %s", ip, CASTV2_PORT, strerror(errno));
        close(client->sock_fd);
        client->sock_fd = -1;
        return false;
    }

    /* Initialize OpenSSL */
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        log_error("CastV2: SSL_CTX_new() failed");
        close(client->sock_fd);
        client->sock_fd = -1;
        return false;
    }

    /* Chromecast uses self-signed certificates */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    client->ssl_ctx = ctx;

    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        log_error("CastV2: SSL_new() failed");
        SSL_CTX_free(ctx);
        close(client->sock_fd);
        client->sock_fd = -1;
        client->ssl_ctx = NULL;
        return false;
    }

    SSL_set_fd(ssl, client->sock_fd);
    if (SSL_connect(ssl) != 1) {
        log_error("CastV2: SSL_connect() failed");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(client->sock_fd);
        client->sock_fd = -1;
        client->ssl_ctx = NULL;
        return false;
    }

    client->ssl = ssl;
    log_info("CastV2: TLS connected to %s:%d", ip, CASTV2_PORT);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SEND / RECEIVE MESSAGES                                            */
/* ═══════════════════════════════════════════════════════════════════ */

/* Thread-safe send: protects SSL_write with mutex when heartbeat is active */
static bool castv2_send(castv2_client_t* client, const char* dest_id,
                        const char* ns, const char* payload) {
    uint8_t buf[4096];
    int len = castv2_build_message(buf, sizeof(buf), "sender-0", dest_id,
                                   ns, payload);
    if (len < 0) {
        log_error("CastV2: message build failed");
        return false;
    }

    /* Lock mutex if heartbeat is running (concurrent SSL_write protection) */
    bool locked = client->heartbeat_running;
    if (locked) pthread_mutex_lock(&client->hb_mutex);

    SSL* ssl = (SSL*)client->ssl;
    int written = SSL_write(ssl, buf, len);

    if (locked) pthread_mutex_unlock(&client->hb_mutex);

    if (written != len) {
        log_error("CastV2: SSL_write failed (%d/%d)", written, len);
        return false;
    }
    return true;
}

/* Read a single framed message. Returns payload JSON in buf (null-terminated).
 * Returns number of bytes read, or -1 on error. */
static int castv2_recv(castv2_client_t* client, char* payload_buf,
                       int payload_buf_size) {
    SSL* ssl = (SSL*)client->ssl;

    /* Read 4-byte length header */
    uint8_t hdr[4];
    int rd = SSL_read(ssl, hdr, 4);
    if (rd != 4) return -1;

    uint32_t msg_len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                       ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];

    if (msg_len > 65536) return -1; /* Sanity check */

    uint8_t* msg = (uint8_t*)malloc(msg_len);
    if (!msg) return -1;

    /* Read full message */
    int total = 0;
    while ((uint32_t)total < msg_len) {
        rd = SSL_read(ssl, msg + total, (int)(msg_len - (uint32_t)total));
        if (rd <= 0) { free(msg); return -1; }
        total += rd;
    }

    /* Parse protobuf to extract payload_utf8 (field 6).
     * Simple linear scan: look for field tags. */
    int pos = 0;
    payload_buf[0] = '\0';
    while (pos < total) {
        uint64_t tag_val;
        int n = castv2_decode_varint(msg + pos, total - pos, &tag_val);
        if (n < 0) break;
        pos += n;

        int field_num = (int)(tag_val >> 3);
        int wire_type = (int)(tag_val & 0x07);

        if (wire_type == 0) {
            /* Varint */
            uint64_t dummy;
            n = castv2_decode_varint(msg + pos, total - pos, &dummy);
            if (n < 0) break;
            pos += n;
        } else if (wire_type == 2) {
            /* Length-delimited */
            uint64_t slen;
            n = castv2_decode_varint(msg + pos, total - pos, &slen);
            if (n < 0) break;
            pos += n;
            if (field_num == 6 && (int)slen < payload_buf_size - 1) {
                /* payload_utf8 */
                memcpy(payload_buf, msg + pos, (size_t)slen);
                payload_buf[slen] = '\0';
            }
            pos += (int)slen;
        } else {
            break; /* Unknown wire type */
        }
    }

    free(msg);
    return (int)strlen(payload_buf);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  HEARTBEAT THREAD                                                   */
/* ═══════════════════════════════════════════════════════════════════ */

static void* heartbeat_thread_func(void* arg) {
    castv2_client_t* client = (castv2_client_t*)arg;
    const char* ping_json = "{\"type\":\"PING\"}";
    int cycle = 0;

    while (client->heartbeat_running) {
        sleep(CASTV2_HEARTBEAT_SEC);
        if (!client->heartbeat_running) break;

        pthread_mutex_lock(&client->hb_mutex);

        /* Ping receiver-0 */
        castv2_send(client, "receiver-0", CASTV2_NS_HEARTBEAT, ping_json);

        /* Ping transport if connected */
        if (client->transport_id[0]) {
            castv2_send(client, client->transport_id,
                       CASTV2_NS_HEARTBEAT, ping_json);
        }

        /* Every 30s: GET_STATUS to prevent Chromecast screensaver */
        cycle++;
        if (cycle % 6 == 0 && client->transport_id[0]) {
            char status_json[128];
            snprintf(status_json, sizeof(status_json),
                     "{\"type\":\"GET_STATUS\",\"requestId\":%d}",
                     client->request_id++);
            castv2_send(client, "receiver-0", CASTV2_NS_RECEIVER, status_json);
        }

        /* Every 60s: re-send URL to DashCast to keep it active */
        if (cycle % 12 == 0 && client->transport_id[0] && client->stream_url[0]) {
            char url_json[512];
            snprintf(url_json, sizeof(url_json),
                     "{\"url\":\"%s\",\"force\":true}", client->stream_url);
            castv2_send(client, client->transport_id,
                       CASTV2_NS_DASHCAST, url_json);
        }

        pthread_mutex_unlock(&client->hb_mutex);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  LOCAL IP DETECTION                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

bool castv2_get_local_ip(char* ip_out) {
    /* UDP connect to 8.8.8.8:53 (no data sent) to determine local IP */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if (getsockname(sock, (struct sockaddr*)&local, &local_len) < 0) {
        close(sock);
        return false;
    }

    inet_ntop(AF_INET, &local.sin_addr, ip_out, 64);
    close(sock);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  mDNS DISCOVERY (wrapper around existing cast_server_build_mdns_query)     */
/* ═══════════════════════════════════════════════════════════════════ */

bool castv2_discover_device(char* ip_out, const char* name_filter,
                            int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 3000;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(5353);
    bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr));

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    /* Build and send query */
    uint8_t query[64];
    int query_len = cast_server_build_mdns_query(query, sizeof(query));
    if (query_len < 0) { close(sock); return false; }

    struct sockaddr_in mdns_addr;
    memset(&mdns_addr, 0, sizeof(mdns_addr));
    mdns_addr.sin_family = AF_INET;
    mdns_addr.sin_addr.s_addr = inet_addr("224.0.0.251");
    mdns_addr.sin_port = htons(5353);

    sendto(sock, query, (size_t)query_len, 0,
           (struct sockaddr*)&mdns_addr, sizeof(mdns_addr));

    log_info("CastV2: searching for Chromecast%s%s%s...",
             name_filter ? " '" : "", name_filter ? name_filter : "",
             name_filter ? "'" : "");

    /* Set non-blocking for select() */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct timeval start, now;
    gettimeofday(&start, NULL);
    uint8_t resp[4096];
    bool found = false;

    while (!found) {
        gettimeofday(&now, NULL);
        int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                            (now.tv_usec - start.tv_usec) / 1000);
        if (elapsed >= timeout_ms) break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv;
        int remaining = timeout_ms - elapsed;
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;

        if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, resp, sizeof(resp), 0,
                             (struct sockaddr*)&from, &from_len);
        if (n < 12) continue;

        uint16_t dns_flags = (uint16_t)((resp[2] << 8) | resp[3]);
        if (!(dns_flags & 0x8000)) continue;

        /* Extract IP from sender address as fallback */
        char sender_ip[64];
        inet_ntop(AF_INET, &from.sin_addr, sender_ip, sizeof(sender_ip));

        /* Parse for TXT fn= and A record */
        uint16_t qdcount = (uint16_t)((resp[4] << 8) | resp[5]);
        uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
        uint16_t nscount = (uint16_t)((resp[8] << 8) | resp[9]);
        uint16_t arcount = (uint16_t)((resp[10] << 8) | resp[11]);

        int pos = 12;
        /* Skip questions */
        for (int i = 0; i < qdcount && pos < (int)n; i++) {
            while (pos < (int)n) {
                if (resp[pos] == 0) { pos++; break; }
                if ((resp[pos] & 0xC0) == 0xC0) { pos += 2; goto next_q; }
                pos += 1 + resp[pos];
            }
            next_q:
            pos += 4;
        }

        int total_rr = ancount + nscount + arcount;
        char device_ip[64] = "";
        char display_name[128] = "";

        /* Simple name buffer for dns_read_name - we use inline parsing */
        for (int rr = 0; rr < total_rr && pos + 10 < (int)n; rr++) {
            /* Skip RR name */
            while (pos < (int)n) {
                if (resp[pos] == 0) { pos++; break; }
                if ((resp[pos] & 0xC0) == 0xC0) { pos += 2; break; }
                pos += 1 + resp[pos];
            }
            if (pos + 10 > (int)n) break;

            uint16_t rtype = (uint16_t)((resp[pos] << 8) | resp[pos + 1]);
            uint16_t rdlen = (uint16_t)((resp[pos + 8] << 8) | resp[pos + 9]);
            int rdata_pos = pos + 10;
            pos = rdata_pos + rdlen;

            if (rtype == 1 && rdlen == 4) {
                snprintf(device_ip, sizeof(device_ip), "%d.%d.%d.%d",
                         resp[rdata_pos], resp[rdata_pos + 1],
                         resp[rdata_pos + 2], resp[rdata_pos + 3]);
            } else if (rtype == 16 && rdlen > 0) {
                int tpos = rdata_pos;
                while (tpos < rdata_pos + rdlen && tpos < (int)n) {
                    uint8_t txt_len = resp[tpos++];
                    if (txt_len == 0) break;
                    if (tpos + txt_len > (int)n) break;
                    if (txt_len > 3 && resp[tpos] == 'f' && resp[tpos + 1] == 'n' &&
                        resp[tpos + 2] == '=') {
                        int name_len = txt_len - 3;
                        if (name_len > (int)sizeof(display_name) - 1)
                            name_len = (int)sizeof(display_name) - 1;
                        memcpy(display_name, resp + tpos + 3, (size_t)name_len);
                        display_name[name_len] = '\0';
                    }
                    tpos += txt_len;
                }
            }
        }

        /* Use sender IP as fallback if no A record */
        if (!device_ip[0] && sender_ip[0]) {
            snprintf(device_ip, sizeof(device_ip), "%s", sender_ip);
        }

        if (device_ip[0]) {
            /* Apply name filter if specified */
            if (name_filter && name_filter[0] && display_name[0]) {
                if (strcasestr(display_name, name_filter) == NULL) {
                    continue; /* Name doesn't match filter */
                }
            }
            snprintf(ip_out, 64, "%s", device_ip);
            log_info("CastV2: found device '%s' at %s",
                     display_name[0] ? display_name : "Unknown", device_ip);
            found = true;
        }
    }

    close(sock);
    return found;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  CAST PROTOCOL SEQUENCE                                             */
/* ═══════════════════════════════════════════════════════════════════ */

/* Simple JSON string search (no JSON parser dependency) */
static bool json_find_string(const char* json, const char* key,
                             char* value, int value_size) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    /* Skip : and whitespace */
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < value_size - 1) {
        value[i++] = *p++;
    }
    value[i] = '\0';
    return i > 0;
}

bool castv2_connect_and_cast(castv2_client_t* client, const char* device_ip,
                             const char* stream_url) {
    memset(client, 0, sizeof(*client));
    client->sock_fd = -1;
    client->request_id = 1;
    snprintf(client->device_ip, sizeof(client->device_ip), "%s", device_ip);
    snprintf(client->stream_url, sizeof(client->stream_url), "%s", stream_url);

    /* Step 1: TLS connect */
    client->state = CASTV2_STATE_CONNECTING;
    if (!castv2_tls_connect(client, device_ip)) {
        client->state = CASTV2_STATE_ERROR;
        return false;
    }
    client->state = CASTV2_STATE_CONNECTED;

    /* Step 2: Initialize mutex for thread-safe SSL writes */
    pthread_mutex_init(&client->hb_mutex, NULL);

    /* Step 3: CONNECT to receiver-0 */
    if (!castv2_send(client, "receiver-0", CASTV2_NS_CONNECTION,
                     "{\"type\":\"CONNECT\",\"origin\":{}}")) {
        client->state = CASTV2_STATE_ERROR;
        return false;
    }
    log_info("CastV2: CONNECT sent to receiver-0");

    /* Step 3b: STOP any existing app to force a fresh DashCast launch */
    {
        char stop_json[128];
        snprintf(stop_json, sizeof(stop_json),
                 "{\"type\":\"STOP\",\"requestId\":%d}",
                 client->request_id++);
        castv2_send(client, "receiver-0", CASTV2_NS_RECEIVER, stop_json);
        log_info("CastV2: STOP sent (clearing previous session)");
        /* Give Chromecast time to stop the app */
        usleep(1500000);
        /* Drain any pending messages */
        char drain[4096];
        while (castv2_recv(client, drain, sizeof(drain)) > 0) { }
    }

    /* Step 4: LAUNCH DashCast */
    client->state = CASTV2_STATE_LAUNCHING;
    char launch_json[256];
    snprintf(launch_json, sizeof(launch_json),
             "{\"type\":\"LAUNCH\",\"appId\":\"%s\",\"requestId\":%d}",
             CASTV2_DASHCAST_APPID, client->request_id++);

    if (!castv2_send(client, "receiver-0", CASTV2_NS_RECEIVER, launch_json)) {
        client->state = CASTV2_STATE_ERROR;
        return false;
    }
    log_info("CastV2: LAUNCH DashCast sent (appId=%s)", CASTV2_DASHCAST_APPID);

    /* Step 5: Wait for RECEIVER_STATUS to get transportId.
     * DashCast may take several seconds to launch. We read messages
     * and respond to PINGs while waiting. */
    char payload[4096];
    bool got_transport = false;

    for (int attempt = 0; attempt < 60; attempt++) {
        int rd = castv2_recv(client, payload, sizeof(payload));
        if (rd <= 0) {
            /* Timeout — send a PING to keep connection alive */
            castv2_send(client, "receiver-0", CASTV2_NS_HEARTBEAT,
                       "{\"type\":\"PING\"}");
            continue;
        }

        log_debug("CastV2: recv [%d]: %.120s%s", attempt, payload,
                  (int)strlen(payload) > 120 ? "..." : "");

        /* Respond to PING from Chromecast */
        if (strstr(payload, "\"PING\"")) {
            castv2_send(client, "receiver-0", CASTV2_NS_HEARTBEAT,
                       "{\"type\":\"PONG\"}");
            continue;
        }

        /* Handle PONG silently */
        if (strstr(payload, "\"PONG\"")) continue;

        /* Handle CLOSE — Chromecast rejected us */
        if (strstr(payload, "\"CLOSE\"")) {
            log_error("CastV2: received CLOSE from Chromecast");
            client->state = CASTV2_STATE_ERROR;
            return false;
        }

        /* Look for RECEIVER_STATUS with transportId */
        if (strstr(payload, "transportId")) {
            char tid[128] = "";
            if (json_find_string(payload, "transportId", tid, sizeof(tid))) {
                snprintf(client->transport_id, sizeof(client->transport_id), "%s", tid);
                log_info("CastV2: got transportId: %s", tid);
                got_transport = true;
                break;
            }
        }

        /* Request status if we haven't got transport yet */
        if (attempt > 0 && attempt % 10 == 0) {
            char status_json[128];
            snprintf(status_json, sizeof(status_json),
                     "{\"type\":\"GET_STATUS\",\"requestId\":%d}",
                     client->request_id++);
            castv2_send(client, "receiver-0", CASTV2_NS_RECEIVER, status_json);
            log_info("CastV2: requesting status (attempt %d)...", attempt);
        }
    }

    if (!got_transport) {
        log_error("CastV2: failed to get transportId from RECEIVER_STATUS");
        client->state = CASTV2_STATE_ERROR;
        return false;
    }

    /* Step 6: CONNECT to transport */
    if (!castv2_send(client, client->transport_id, CASTV2_NS_CONNECTION,
                     "{\"type\":\"CONNECT\",\"origin\":{}}")) {
        client->state = CASTV2_STATE_ERROR;
        return false;
    }
    log_info("CastV2: CONNECT sent to transport %s", client->transport_id);

    /* Wait for transport to be ready — drain messages and respond to PINGs */
    usleep(1000000); /* 1s initial wait */
    for (int drain = 0; drain < 10; drain++) {
        int drd = castv2_recv(client, payload, sizeof(payload));
        if (drd <= 0) break;
        log_debug("CastV2: transport drain [%d]: %.80s", drain, payload);
        if (strstr(payload, "\"PING\"")) {
            castv2_send(client, client->transport_id,
                       CASTV2_NS_HEARTBEAT, "{\"type\":\"PONG\"}");
        }
    }

    /* Step 7: Send URL to DashCast (with retries) */
    char url_json[512];
    snprintf(url_json, sizeof(url_json),
             "{\"url\":\"%s\",\"force\":true,\"reload\":0,\"reloadTime\":0}",
             stream_url);

    bool url_sent = false;
    for (int retry = 0; retry < 5; retry++) {
        if (!castv2_send(client, client->transport_id,
                         CASTV2_NS_DASHCAST, url_json)) {
            log_warning("CastV2: URL send attempt %d failed", retry + 1);
            usleep(1000000);
            continue;
        }
        log_info("CastV2: URL sent to DashCast (attempt %d)", retry + 1);
        url_sent = true;

        /* Wait and check if DashCast acknowledged */
        usleep(2000000); /* 2s for DashCast to load */

        /* Re-send URL to be sure (DashCast may need it twice) */
        if (retry == 0) {
            castv2_send(client, client->transport_id,
                       CASTV2_NS_DASHCAST, url_json);
            log_info("CastV2: URL re-sent for confirmation");
        }
        break;
    }

    if (!url_sent) {
        log_error("CastV2: failed to send URL after retries");
        client->state = CASTV2_STATE_ERROR;
        return false;
    }

    /* Step 8: Start heartbeat thread (now that connection is fully established) */
    client->heartbeat_running = true;
    if (pthread_create(&client->heartbeat_thread, NULL,
                       heartbeat_thread_func, client) != 0) {
        log_error("CastV2: failed to create heartbeat thread");
        client->heartbeat_running = false;
    }

    client->state = CASTV2_STATE_RUNNING;
    log_info("CastV2: URL sent to DashCast: %s", stream_url);
    log_info("CastV2: Chromecast should now display Phosphoric");

    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  CLEANUP                                                            */
/* ═══════════════════════════════════════════════════════════════════ */

void castv2_disconnect(castv2_client_t* client) {
    if (client->state == CASTV2_STATE_IDLE) return;

    log_info("CastV2: disconnecting...");

    /* Stop heartbeat thread first — set flag then cancel sleep */
    if (client->heartbeat_running) {
        client->heartbeat_running = false;
        /* Cancel the thread if it's sleeping */
        pthread_cancel(client->heartbeat_thread);
        pthread_join(client->heartbeat_thread, NULL);
    }

    /* Send STOP to receiver if we have a transport */
    if (client->ssl && client->transport_id[0]) {
        char stop_json[64];
        snprintf(stop_json, sizeof(stop_json),
                 "{\"type\":\"STOP\",\"requestId\":%d}", client->request_id++);
        castv2_send(client, "receiver-0", CASTV2_NS_RECEIVER, stop_json);
    }

    /* Close socket first to unblock any SSL operations */
    if (client->sock_fd >= 0) {
        shutdown(client->sock_fd, SHUT_RDWR);
    }

    /* SSL cleanup (non-blocking since socket is shut down) */
    if (client->ssl) {
        SSL_free((SSL*)client->ssl);
        client->ssl = NULL;
    }

    if (client->ssl_ctx) {
        SSL_CTX_free((SSL_CTX*)client->ssl_ctx);
        client->ssl_ctx = NULL;
    }

    if (client->sock_fd >= 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
    }

    pthread_mutex_destroy(&client->hb_mutex);

    client->state = CASTV2_STATE_IDLE;
    log_info("CastV2: disconnected");
}

#endif /* HAS_CAST */
