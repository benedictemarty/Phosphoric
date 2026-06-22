/**
 * @file cast_server.c
 * @brief MJPEG HTTP streaming server + mDNS Chromecast discovery
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-28
 * @version 1.2.0-alpha
 *
 * Provides an HTTP MJPEG stream at /stream and an HTML viewer page at /.
 * Uses select() for non-blocking I/O with up to 8 concurrent clients.
 * mDNS discovery sends DNS-SD queries for _googlecast._tcp.local.
 */

#ifdef HAS_CAST

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* stb_image_write for JPEG encoding. The implementation lives in a single
 * TU (src/video/stb_image_write_impl.c); here we only need the declarations. */
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb_image_write.h"

#include "network/cast_server.h"
#include "utils/logging.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  JPEG ENCODING CALLBACK                                            */
/* ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   capacity;
} jpeg_buffer_t;

static void jpeg_write_callback(void* context, void* data, int size) {
    jpeg_buffer_t* buf = (jpeg_buffer_t*)context;
    size_t sz = (size_t)size;
    if (buf->len + sz > buf->capacity) {
        size_t new_cap = (buf->len + sz) * 2;
        uint8_t* new_data = (uint8_t*)realloc(buf->data, new_cap);
        if (!new_data) return;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->len, data, sz);
    buf->len += sz;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  UPSCALE                                                            */
/* ═══════════════════════════════════════════════════════════════════ */

void cast_server_upscale_nearest(const uint8_t* src, int src_w, int src_h,
                          uint8_t* dst, int factor) {
    int dst_w = src_w * factor;
    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            const uint8_t* sp = src + (y * src_w + x) * 3;
            for (int fy = 0; fy < factor; fy++) {
                for (int fx = 0; fx < factor; fx++) {
                    int dy = y * factor + fy;
                    int dx = x * factor + fx;
                    uint8_t* dp = dst + (dy * dst_w + dx) * 3;
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  WAV AUDIO STREAMING                                                */
/* ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Build a WAV header for infinite streaming (44 bytes)
 *
 * RIFF header with PCM format: mono, 44100 Hz, 16-bit.
 * Data chunk size set to 0x7FFFFFFF for continuous streaming.
 */
int cast_server_build_wav_header(uint8_t* buf, size_t buf_size) {
    if (buf_size < 44) return -1;

    int sample_rate = CAST_AUDIO_RATE;
    int num_channels = 1;
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;

    /* RIFF header */
    memcpy(buf + 0, "RIFF", 4);
    /* File size - 8 (infinite: 0x7FFFFFFF + 36) */
    uint32_t riff_size = 0x7FFFFFFF;
    memcpy(buf + 4, &riff_size, 4);
    memcpy(buf + 8, "WAVE", 4);

    /* fmt sub-chunk */
    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(buf + 16, &fmt_size, 4);
    uint16_t audio_format = 1; /* PCM */
    memcpy(buf + 20, &audio_format, 2);
    uint16_t channels = (uint16_t)num_channels;
    memcpy(buf + 22, &channels, 2);
    uint32_t sr = (uint32_t)sample_rate;
    memcpy(buf + 24, &sr, 4);
    uint32_t br = (uint32_t)byte_rate;
    memcpy(buf + 28, &br, 4);
    uint16_t ba = (uint16_t)block_align;
    memcpy(buf + 32, &ba, 2);
    uint16_t bps = (uint16_t)bits_per_sample;
    memcpy(buf + 34, &bps, 2);

    /* data sub-chunk */
    memcpy(buf + 36, "data", 4);
    uint32_t data_size = 0x7FFFFFFF;
    memcpy(buf + 40, &data_size, 4);

    return 44;
}

/**
 * @brief Push stereo audio samples to the ring buffer (stereo → mono downmix)
 */
void cast_server_push_audio(cast_server_t* server, const int16_t* stereo_samples,
                             size_t num_samples) {
    if (!server->active) return;

    pthread_mutex_lock(&server->audio_mutex);
    for (size_t i = 0; i < num_samples; i++) {
        /* Stereo to mono: average L and R */
        int32_t left = stereo_samples[i * 2];
        int32_t right = stereo_samples[i * 2 + 1];
        server->audio_ring[server->audio_write_pos] = (int16_t)((left + right) / 2);
        server->audio_write_pos = (server->audio_write_pos + 1) % CAST_AUDIO_RING_SAMPLES;
    }
    pthread_mutex_unlock(&server->audio_mutex);
}

static void add_audio_client(cast_server_t* server, int fd) {
    if (server->num_audio_clients >= CAST_MAX_AUDIO_CLIENTS) {
        close(fd);
        return;
    }
    int idx = server->num_audio_clients;
    server->audio_clients[idx] = fd;
    /* Start reading from current write position (no old audio) */
    pthread_mutex_lock(&server->audio_mutex);
    server->audio_read_pos[idx] = server->audio_write_pos;
    pthread_mutex_unlock(&server->audio_mutex);
    server->num_audio_clients++;
    log_info("Cast: audio client connected (fd=%d, total=%d)", fd, server->num_audio_clients);
}

static void remove_audio_client(cast_server_t* server, int index) {
    if (index < 0 || index >= server->num_audio_clients) return;
    close(server->audio_clients[index]);
    log_info("Cast: audio client disconnected (fd=%d)", server->audio_clients[index]);
    for (int i = index; i < server->num_audio_clients - 1; i++) {
        server->audio_clients[i] = server->audio_clients[i + 1];
        server->audio_read_pos[i] = server->audio_read_pos[i + 1];
    }
    server->num_audio_clients--;
}

static void broadcast_audio(cast_server_t* server) {
    if (server->num_audio_clients == 0) return;

    pthread_mutex_lock(&server->audio_mutex);
    uint32_t write_pos = server->audio_write_pos;
    pthread_mutex_unlock(&server->audio_mutex);

    for (int i = server->num_audio_clients - 1; i >= 0; i--) {
        uint32_t read_pos = server->audio_read_pos[i];
        uint32_t avail;
        if (write_pos >= read_pos) {
            avail = write_pos - read_pos;
        } else {
            avail = CAST_AUDIO_RING_SAMPLES - read_pos + write_pos;
        }
        if (avail == 0) continue;

        /* Send available samples in up to 2 chunks (ring wrap-around) */
        bool error = false;
        if (write_pos >= read_pos) {
            /* Single contiguous chunk */
            ssize_t sent = send(server->audio_clients[i],
                                server->audio_ring + read_pos,
                                (size_t)(avail * sizeof(int16_t)), MSG_NOSIGNAL);
            if (sent <= 0) error = true;
        } else {
            /* Two chunks: read_pos to end, then 0 to write_pos */
            uint32_t chunk1 = CAST_AUDIO_RING_SAMPLES - read_pos;
            ssize_t s1 = send(server->audio_clients[i],
                              server->audio_ring + read_pos,
                              (size_t)(chunk1 * sizeof(int16_t)), MSG_NOSIGNAL);
            if (s1 <= 0) {
                error = true;
            } else if (write_pos > 0) {
                ssize_t s2 = send(server->audio_clients[i],
                                  server->audio_ring,
                                  (size_t)(write_pos * sizeof(int16_t)), MSG_NOSIGNAL);
                if (s2 <= 0) error = true;
            }
        }

        if (error) {
            remove_audio_client(server, i);
        } else {
            server->audio_read_pos[i] = write_pos;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  HTTP RESPONSES                                                     */
/* ═══════════════════════════════════════════════════════════════════ */

/* Page HTML avec fallback JS pour navigateurs sans support MJPEG (Chromecast).
 * Tente d'abord le flux MJPEG natif (/stream). Si l'image ne se charge pas
 * en 2s, bascule sur un rafraichissement JS via /snapshot (un JPEG par requete). */
static const char* HTML_PAGE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html>\n"
    "<html><head><title>ORIC-1 Cast</title>\n"
    "<style>\n"
    "  body { margin: 0; background: #000; display: flex;\n"
    "         justify-content: center; align-items: center;\n"
    "         min-height: 100vh; overflow: hidden; }\n"
    "  img  { max-width: 100vw; max-height: 100vh;\n"
    "         image-rendering: pixelated;\n"
    "         image-rendering: crisp-edges; }\n"
    "</style></head><body>\n"
    "<img id=\"screen\" alt=\"ORIC-1\">\n"
    "<script>\n"
    "/* Video: snapshot polling (50ms) */\n"
    "var img = document.getElementById('screen');\n"
    "function refresh() {\n"
    "  var i = new Image();\n"
    "  i.onload = function() {\n"
    "    img.src = i.src;\n"
    "    setTimeout(refresh, 50);\n"
    "  };\n"
    "  i.onerror = function() { setTimeout(refresh, 500); };\n"
    "  i.src = '/snapshot?t=' + Date.now();\n"
    "}\n"
    "refresh();\n"
    "\n"
    "/* Audio: Web Audio API streaming with auto-reconnect */\n"
    "var actx = null;\n"
    "var nextTime = 0;\n"
    "function connectAudio() {\n"
    "  if (!actx) {\n"
    "    actx = new (window.AudioContext || window.webkitAudioContext)(\n"
    "      {sampleRate: 44100});\n"
    "  }\n"
    "  nextTime = 0;\n"
    "  fetch('/audio').then(function(resp) {\n"
    "    var reader = resp.body.getReader();\n"
    "    var hdrSkip = 0;\n"
    "    var leftover = new Uint8Array(0);\n"
    "    function pump() {\n"
    "      reader.read().then(function(result) {\n"
    "        if (result.done) {\n"
    "          setTimeout(connectAudio, 1000);\n"
    "          return;\n"
    "        }\n"
    "        var chunk = result.value;\n"
    "        if (hdrSkip < 44) {\n"
    "          var skip = Math.min(44 - hdrSkip, chunk.length);\n"
    "          chunk = chunk.slice(skip);\n"
    "          hdrSkip += skip;\n"
    "        }\n"
    "        if (chunk.length === 0) { pump(); return; }\n"
    "        if (leftover.length > 0) {\n"
    "          var m = new Uint8Array(leftover.length + chunk.length);\n"
    "          m.set(leftover);\n"
    "          m.set(chunk, leftover.length);\n"
    "          chunk = m;\n"
    "          leftover = new Uint8Array(0);\n"
    "        }\n"
    "        var usable = chunk.length & ~1;\n"
    "        if (chunk.length > usable)\n"
    "          leftover = chunk.slice(usable);\n"
    "        var nS = usable / 2;\n"
    "        if (nS === 0) { pump(); return; }\n"
    "        var buf = actx.createBuffer(1, nS, 44100);\n"
    "        var ch = buf.getChannelData(0);\n"
    "        var dv = new DataView(\n"
    "          chunk.buffer, chunk.byteOffset, usable);\n"
    "        for (var s = 0; s < nS; s++)\n"
    "          ch[s] = dv.getInt16(s * 2, true) / 32768.0;\n"
    "        var now = actx.currentTime;\n"
    "        if (nextTime < now) nextTime = now + 0.02;\n"
    "        var src = actx.createBufferSource();\n"
    "        src.buffer = buf;\n"
    "        src.connect(actx.destination);\n"
    "        src.start(nextTime);\n"
    "        nextTime += buf.duration;\n"
    "        if (nextTime - now > 0.5) nextTime = now + 0.02;\n"
    "        pump();\n"
    "      }).catch(function() {\n"
    "        setTimeout(connectAudio, 2000);\n"
    "      });\n"
    "    }\n"
    "    pump();\n"
    "  }).catch(function() {\n"
    "    setTimeout(connectAudio, 2000);\n"
    "  });\n"
    "}\n"
    "connectAudio();\n"
    "document.addEventListener('click', function() {\n"
    "  if (actx && actx.state === 'suspended') actx.resume();\n"
    "}, {once: true});\n"
    "</script>\n"
    "</body></html>\n";

static const char* MJPEG_HEADER =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static const char* MJPEG_BOUNDARY = "--frame\r\n";

/* ═══════════════════════════════════════════════════════════════════ */
/*  CLIENT MANAGEMENT                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void add_client(cast_server_t* server, int fd) {
    if (server->num_clients >= CAST_MAX_CLIENTS) {
        close(fd);
        return;
    }
    server->clients[server->num_clients++] = fd;
    log_info("Cast: client connected (fd=%d, total=%d)", fd, server->num_clients);
}

static void remove_client(cast_server_t* server, int index) {
    if (index < 0 || index >= server->num_clients) return;
    close(server->clients[index]);
    log_info("Cast: client disconnected (fd=%d)", server->clients[index]);
    /* Shift remaining clients down */
    for (int i = index; i < server->num_clients - 1; i++) {
        server->clients[i] = server->clients[i + 1];
    }
    server->num_clients--;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  HTTP REQUEST PARSING                                               */
/* ═══════════════════════════════════════════════════════════════════ */

static void route_http_request(cast_server_t* server, int client_fd) {
    char request[1024];
    ssize_t n = recv(client_fd, request, sizeof(request) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    request[n] = '\0';

    if (strstr(request, "GET /audio") != NULL) {
        /* WAV audio stream — send WAV header and keep for streaming */
        uint8_t wav_hdr[44];
        cast_server_build_wav_header(wav_hdr, sizeof(wav_hdr));

        const char* http_hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: audio/wav\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        send(client_fd, http_hdr, strlen(http_hdr), MSG_NOSIGNAL);
        send(client_fd, wav_hdr, 44, MSG_NOSIGNAL);
        set_nonblocking(client_fd);
        add_audio_client(server, client_fd);
    } else if (strstr(request, "GET /stream") != NULL) {
        /* MJPEG stream — send header and keep connection for streaming */
        send(client_fd, MJPEG_HEADER, strlen(MJPEG_HEADER), MSG_NOSIGNAL);
        set_nonblocking(client_fd);
        add_client(server, client_fd);
    } else if (strstr(request, "GET /snapshot") != NULL) {
        /* Single JPEG snapshot — encode current frame, send, close.
         * Used as fallback by Chromecast browser (no MJPEG support). */
        uint8_t* frame_copy = (uint8_t*)malloc(CAST_FRAME_W * CAST_FRAME_H * 3);
        if (!frame_copy) { close(client_fd); return; }

        pthread_mutex_lock(&server->mutex);
        memcpy(frame_copy, server->upscaled, CAST_FRAME_W * CAST_FRAME_H * 3);
        pthread_mutex_unlock(&server->mutex);

        jpeg_buffer_t jbuf;
        jbuf.capacity = CAST_FRAME_W * CAST_FRAME_H * 3;
        jbuf.data = (uint8_t*)malloc((size_t)jbuf.capacity);
        jbuf.len = 0;
        if (!jbuf.data) { free(frame_copy); close(client_fd); return; }

        stbi_write_jpg_to_func(jpeg_write_callback, &jbuf,
                               CAST_FRAME_W, CAST_FRAME_H, 3,
                               frame_copy, CAST_JPEG_QUALITY);
        free(frame_copy);

        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache, no-store\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n", jbuf.len);

        send(client_fd, hdr, (size_t)hlen, MSG_NOSIGNAL);
        if (jbuf.len > 0) {
            send(client_fd, jbuf.data, jbuf.len, MSG_NOSIGNAL);
        }
        free(jbuf.data);
        close(client_fd);
    } else if (strstr(request, "GET /") != NULL) {
        /* HTML page — send and close */
        send(client_fd, HTML_PAGE, strlen(HTML_PAGE), MSG_NOSIGNAL);
        close(client_fd);
    } else {
        close(client_fd);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  FRAME BROADCAST                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

static void broadcast_frame(cast_server_t* server) {
    if (server->num_clients == 0) return;

    /* Encode JPEG from upscaled buffer (outside mutex) */
    jpeg_buffer_t jbuf;
    jbuf.data = server->jpeg_buf;
    jbuf.len = 0;
    jbuf.capacity = server->jpeg_capacity;

    /* Copy upscaled frame under mutex */
    uint8_t* frame_copy = (uint8_t*)malloc(CAST_FRAME_W * CAST_FRAME_H * 3);
    if (!frame_copy) return;

    pthread_mutex_lock(&server->mutex);
    memcpy(frame_copy, server->upscaled, CAST_FRAME_W * CAST_FRAME_H * 3);
    server->frame_ready = false;
    pthread_mutex_unlock(&server->mutex);

    /* Encode JPEG (no mutex held) */
    stbi_write_jpg_to_func(jpeg_write_callback, &jbuf,
                           CAST_FRAME_W, CAST_FRAME_H, 3,
                           frame_copy, CAST_JPEG_QUALITY);
    free(frame_copy);

    /* Update server's JPEG buffer */
    server->jpeg_buf = jbuf.data;
    server->jpeg_len = jbuf.len;
    server->jpeg_capacity = jbuf.capacity;

    if (jbuf.len == 0) return;

    /* Build MJPEG frame header */
    char header[128];
    int hlen = snprintf(header, sizeof(header),
                        "%sContent-Type: image/jpeg\r\n"
                        "Content-Length: %zu\r\n\r\n",
                        MJPEG_BOUNDARY, jbuf.len);

    /* Send to all stream clients */
    for (int i = server->num_clients - 1; i >= 0; i--) {
        ssize_t s1 = send(server->clients[i], header, (size_t)hlen, MSG_NOSIGNAL);
        ssize_t s2 = 0;
        if (s1 > 0) {
            s2 = send(server->clients[i], jbuf.data, jbuf.len, MSG_NOSIGNAL);
        }
        if (s1 <= 0 || s2 <= 0) {
            remove_client(server, i);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SERVER THREAD                                                      */
/* ═══════════════════════════════════════════════════════════════════ */

static void* server_thread_func(void* arg) {
    cast_server_t* server = (cast_server_t*)arg;

    while (server->thread_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server->listen_fd, &readfds);
        int maxfd = server->listen_fd;

        /* select() with 20ms timeout */
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000;

        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(server->listen_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server->listen_fd,
                                   (struct sockaddr*)&client_addr, &addr_len);
            if (client_fd >= 0) {
                route_http_request(server, client_fd);
            }
        }

        /* Broadcast frame if ready */
        bool ready;
        pthread_mutex_lock(&server->mutex);
        ready = server->frame_ready;
        pthread_mutex_unlock(&server->mutex);

        if (ready) {
            broadcast_frame(server);
        }

        /* Broadcast audio to streaming clients */
        broadcast_audio(server);
    }

    /* Close all client connections */
    for (int i = 0; i < server->num_clients; i++) {
        close(server->clients[i]);
    }
    server->num_clients = 0;

    /* Close all audio client connections */
    for (int i = 0; i < server->num_audio_clients; i++) {
        close(server->audio_clients[i]);
    }
    server->num_audio_clients = 0;

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  PUBLIC API                                                         */
/* ═══════════════════════════════════════════════════════════════════ */

bool cast_server_init(cast_server_t* server, uint16_t port) {
    memset(server, 0, sizeof(*server));
    server->port = (port > 0) ? port : CAST_DEFAULT_PORT;
    server->listen_fd = -1;

    /* Initialize mutexes */
    if (pthread_mutex_init(&server->mutex, NULL) != 0) {
        log_error("Cast: failed to create mutex");
        return false;
    }
    if (pthread_mutex_init(&server->audio_mutex, NULL) != 0) {
        log_error("Cast: failed to create audio mutex");
        pthread_mutex_destroy(&server->mutex);
        return false;
    }

    /* Allocate JPEG buffer */
    server->jpeg_capacity = CAST_FRAME_W * CAST_FRAME_H * 3;
    server->jpeg_buf = (uint8_t*)malloc(server->jpeg_capacity);
    if (!server->jpeg_buf) {
        log_error("Cast: failed to allocate JPEG buffer");
        pthread_mutex_destroy(&server->audio_mutex);
        pthread_mutex_destroy(&server->mutex);
        return false;
    }

    /* Create listening socket */
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        log_error("Cast: socket() failed: %s", strerror(errno));
        free(server->jpeg_buf);
        pthread_mutex_destroy(&server->audio_mutex);
        pthread_mutex_destroy(&server->mutex);
        return false;
    }

    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(server->port);

    if (bind(server->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("Cast: bind() failed on port %d: %s",
                  server->port, strerror(errno));
        close(server->listen_fd);
        free(server->jpeg_buf);
        pthread_mutex_destroy(&server->audio_mutex);
        pthread_mutex_destroy(&server->mutex);
        return false;
    }

    if (listen(server->listen_fd, 4) < 0) {
        log_error("Cast: listen() failed: %s", strerror(errno));
        close(server->listen_fd);
        free(server->jpeg_buf);
        pthread_mutex_destroy(&server->audio_mutex);
        pthread_mutex_destroy(&server->mutex);
        return false;
    }

    set_nonblocking(server->listen_fd);

    /* Start server thread */
    server->thread_running = true;
    if (pthread_create(&server->thread, NULL, server_thread_func, server) != 0) {
        log_error("Cast: pthread_create() failed: %s", strerror(errno));
        close(server->listen_fd);
        free(server->jpeg_buf);
        pthread_mutex_destroy(&server->audio_mutex);
        pthread_mutex_destroy(&server->mutex);
        return false;
    }

    server->active = true;
    log_info("Cast: MJPEG server started on http://0.0.0.0:%d/", server->port);
    return true;
}

void cast_server_push_frame(cast_server_t* server, const uint8_t* framebuffer,
                            unsigned int width, unsigned int height) {
    if (!server->active) return;

    pthread_mutex_lock(&server->mutex);
    cast_server_upscale_nearest(framebuffer, width, height,
                         server->upscaled, CAST_UPSCALE_FACTOR);
    server->frame_ready = true;
    pthread_mutex_unlock(&server->mutex);
}

void cast_server_stop(cast_server_t* server) {
    if (!server->active) return;

    server->thread_running = false;
    pthread_join(server->thread, NULL);

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    if (server->jpeg_buf) {
        free(server->jpeg_buf);
        server->jpeg_buf = NULL;
    }

    pthread_mutex_destroy(&server->mutex);
    pthread_mutex_destroy(&server->audio_mutex);
    server->active = false;
    log_info("Cast: server stopped");
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  mDNS / DNS-SD DISCOVERY                                            */
/* ═══════════════════════════════════════════════════════════════════ */

#define MDNS_PORT       5353
#define MDNS_ADDR       "224.0.0.251"
#define MDNS_BUF_SIZE   4096

/**
 * @brief Build a DNS-SD query packet for _googlecast._tcp.local
 *
 * DNS query format:
 *   Header (12 bytes): ID=0, flags=0, QDCOUNT=1
 *   Question: _googlecast._tcp.local, type=PTR(12), class=IN(1)
 */
int cast_server_build_mdns_query(uint8_t* buf, size_t buf_size) {
    if (buf_size < 64) return -1;

    int pos = 0;

    /* DNS Header: ID=0, flags=0x0000, QDCOUNT=1 */
    memset(buf, 0, 12);
    buf[5] = 1; /* QDCOUNT = 1 */
    pos = 12;

    /* QNAME: _googlecast._tcp.local */
    /* Label: _googlecast (11 chars) */
    buf[pos++] = 11;
    memcpy(buf + pos, "_googlecast", 11);
    pos += 11;

    /* Label: _tcp (4 chars) */
    buf[pos++] = 4;
    memcpy(buf + pos, "_tcp", 4);
    pos += 4;

    /* Label: local (5 chars) */
    buf[pos++] = 5;
    memcpy(buf + pos, "local", 5);
    pos += 5;

    /* Null terminator */
    buf[pos++] = 0;

    /* QTYPE = PTR (12) */
    buf[pos++] = 0;
    buf[pos++] = 12;

    /* QCLASS = IN (1) */
    buf[pos++] = 0;
    buf[pos++] = 1;

    return pos;
}

/**
 * @brief Skip a DNS name (handles compression pointers)
 */
static int dns_skip_name(const uint8_t* buf, int pos, int len) {
    while (pos < len) {
        uint8_t label_len = buf[pos];
        if (label_len == 0) {
            return pos + 1;
        }
        if ((label_len & 0xC0) == 0xC0) {
            /* Compression pointer: 2 bytes */
            return pos + 2;
        }
        pos += 1 + label_len;
    }
    return -1;
}

/**
 * @brief Read a DNS name into a buffer, resolving compression pointers
 */
static int dns_read_name(const uint8_t* pkt, int pkt_len, int pos,
                         char* name, int name_size) {
    int out = 0;
    int jump_pos = -1;
    int next_pos = -1;
    int jumps = 0;

    while (pos < pkt_len && jumps < 16) {
        uint8_t label_len = pkt[pos];
        if (label_len == 0) {
            if (next_pos < 0) next_pos = pos + 1;
            break;
        }
        if ((label_len & 0xC0) == 0xC0) {
            if (next_pos < 0) next_pos = pos + 2;
            int offset = ((label_len & 0x3F) << 8) | pkt[pos + 1];
            pos = offset;
            jumps++;
            continue;
        }
        pos++;
        if (out > 0 && out < name_size - 1) {
            name[out++] = '.';
        }
        for (int i = 0; i < label_len && pos < pkt_len && out < name_size - 1; i++) {
            name[out++] = (char)pkt[pos++];
        }
    }
    if (out < name_size) name[out] = '\0';
    (void)jump_pos;
    return (next_pos >= 0) ? next_pos : pos;
}

int cast_server_discover_devices(int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 3000;

    /* Create UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "mDNS: socket() failed: %s\n", strerror(errno));
        return 0;
    }

    /* Enable multicast */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind to mDNS port */
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(MDNS_PORT);

    /* Non-fatal if bind fails (port in use by avahi etc.) */
    bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr));

    /* Join multicast group */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    /* Build and send query */
    uint8_t query[64];
    int query_len = cast_server_build_mdns_query(query, sizeof(query));
    if (query_len < 0) {
        close(sock);
        return 0;
    }

    struct sockaddr_in mdns_addr;
    memset(&mdns_addr, 0, sizeof(mdns_addr));
    mdns_addr.sin_family = AF_INET;
    mdns_addr.sin_addr.s_addr = inet_addr(MDNS_ADDR);
    mdns_addr.sin_port = htons(MDNS_PORT);

    sendto(sock, query, (size_t)query_len, 0,
           (struct sockaddr*)&mdns_addr, sizeof(mdns_addr));

    printf("Searching for Chromecast devices...\n");

    /* Receive responses */
    int found = 0;
    uint8_t resp[MDNS_BUF_SIZE];
    struct timeval start, now;
    gettimeofday(&start, NULL);

    set_nonblocking(sock);

    while (1) {
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

        int ret = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, resp, sizeof(resp), 0,
                             (struct sockaddr*)&from, &from_len);
        if (n < 12) continue;

        /* Parse DNS response header */
        uint16_t flags = (uint16_t)((resp[2] << 8) | resp[3]);
        if (!(flags & 0x8000)) continue; /* Not a response */

        uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
        uint16_t qdcount = (uint16_t)((resp[4] << 8) | resp[5]);

        /* Skip questions */
        int pos = 12;
        for (int i = 0; i < qdcount && pos < (int)n; i++) {
            pos = dns_skip_name(resp, pos, (int)n);
            if (pos < 0) break;
            pos += 4; /* QTYPE + QCLASS */
        }
        if (pos < 0) continue;

        /* Parse answers + additional records */
        uint16_t nscount = (uint16_t)((resp[8] << 8) | resp[9]);
        uint16_t arcount = (uint16_t)((resp[10] << 8) | resp[11]);
        int total_rr = ancount + nscount + arcount;

        char device_name[128] = "";
        char device_ip[64] = "";
        uint16_t device_port = 0;
        char display_name[128] = "";

        for (int rr = 0; rr < total_rr && pos < (int)n; rr++) {
            char rr_name[256];
            pos = dns_read_name(resp, (int)n, pos, rr_name, sizeof(rr_name));
            if (pos < 0 || pos + 10 > (int)n) break;

            uint16_t rtype = (uint16_t)((resp[pos] << 8) | resp[pos + 1]);
            uint16_t rdlen = (uint16_t)((resp[pos + 8] << 8) | resp[pos + 9]);
            int rdata_pos = pos + 10;
            pos = rdata_pos + rdlen;

            if (rtype == 12 && rdlen > 0) {
                /* PTR record: service instance name */
                dns_read_name(resp, (int)n, rdata_pos, device_name, sizeof(device_name));
            } else if (rtype == 33 && rdlen >= 6) {
                /* SRV record: port + target */
                device_port = (uint16_t)((resp[rdata_pos + 4] << 8) | resp[rdata_pos + 5]);
            } else if (rtype == 1 && rdlen == 4) {
                /* A record: IPv4 address */
                snprintf(device_ip, sizeof(device_ip), "%d.%d.%d.%d",
                         resp[rdata_pos], resp[rdata_pos + 1],
                         resp[rdata_pos + 2], resp[rdata_pos + 3]);
            } else if (rtype == 16 && rdlen > 0) {
                /* TXT record: look for fn= key */
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

        if (device_ip[0] || device_name[0]) {
            found++;
            const char* name = display_name[0] ? display_name :
                               (device_name[0] ? device_name : "Unknown");
            printf("  [%d] %s", found, name);
            if (device_ip[0]) printf(" (%s", device_ip);
            if (device_port > 0) printf(":%d", device_port);
            if (device_ip[0]) printf(")");
            printf("\n");
        }
    }

    close(sock);

    if (found == 0) {
        printf("  No Chromecast devices found.\n");
    } else {
        printf("\n  %d device(s) found.\n", found);
    }

    return found;
}

#endif /* HAS_CAST */
