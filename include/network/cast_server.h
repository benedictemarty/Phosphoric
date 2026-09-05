/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cast_server.h
 * @brief MJPEG Cast server + mDNS Chromecast discovery + CASTV2 client
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-28
 * @version 1.3.0-alpha
 *
 * HTTP MJPEG streaming server for casting the emulator display to a browser
 * or Chromecast device. Includes mDNS/DNS-SD discovery and native CASTV2
 * protocol client for zero-dependency Chromecast control.
 *
 * Build with CAST=1 to enable (adds -DHAS_CAST -lpthread -lssl -lcrypto).
 */

#ifndef CAST_SERVER_H
#define CAST_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Upscaled resolution: 240x224 * 3 = 720x672 */
#define CAST_UPSCALE_FACTOR  3
#define CAST_FRAME_W         (240 * CAST_UPSCALE_FACTOR)  /* 720 */
#define CAST_FRAME_H         (224 * CAST_UPSCALE_FACTOR)  /* 672 */
#define CAST_MAX_CLIENTS     8
#define CAST_DEFAULT_PORT    8080
#define CAST_JPEG_QUALITY    80

/* Audio streaming constants */
#define CAST_AUDIO_RATE          44100
#define CAST_AUDIO_RING_SECS     2
#define CAST_AUDIO_RING_SAMPLES  (CAST_AUDIO_RATE * CAST_AUDIO_RING_SECS)
#define CAST_MAX_AUDIO_CLIENTS   4

/* CASTV2 constants */
#define CASTV2_PORT          8009
#define CASTV2_HEARTBEAT_SEC 5
#define CASTV2_DASHCAST_APPID "5C3F0A3C"
#define CASTV2_NS_CONNECTION  "urn:x-cast:com.google.cast.tp.connection"
#define CASTV2_NS_HEARTBEAT   "urn:x-cast:com.google.cast.tp.heartbeat"
#define CASTV2_NS_RECEIVER    "urn:x-cast:com.google.cast.receiver"
#define CASTV2_NS_DASHCAST    "urn:x-cast:es.offd.dashcast"

#ifdef HAS_CAST

#include <pthread.h>

typedef struct cast_server_s {
    uint16_t port;
    bool     active;

    /* Server thread */
    pthread_t thread;
    bool      thread_running;

    /* Shared framebuffer (upscaled RGB888) */
    uint8_t   upscaled[CAST_FRAME_W * CAST_FRAME_H * 3];
    pthread_mutex_t mutex;

    /* JPEG output buffer */
    uint8_t*  jpeg_buf;
    size_t    jpeg_len;
    size_t    jpeg_capacity;

    /* Client sockets */
    int       clients[CAST_MAX_CLIENTS];
    int       num_clients;

    /* Listen socket */
    int       listen_fd;

    /* Frame ready flag */
    bool      frame_ready;

    /* Audio ring buffer (mono PCM 16-bit) */
    int16_t   audio_ring[CAST_AUDIO_RING_SAMPLES];
    uint32_t  audio_write_pos;
    pthread_mutex_t audio_mutex;

    /* Audio streaming clients */
    int       audio_clients[CAST_MAX_AUDIO_CLIENTS];
    uint32_t  audio_read_pos[CAST_MAX_AUDIO_CLIENTS];
    int       num_audio_clients;
} cast_server_t;

/* CASTV2 client state machine */
typedef enum {
    CASTV2_STATE_IDLE = 0,
    CASTV2_STATE_DISCOVERING,
    CASTV2_STATE_CONNECTING,
    CASTV2_STATE_CONNECTED,
    CASTV2_STATE_LAUNCHING,
    CASTV2_STATE_RUNNING,
    CASTV2_STATE_ERROR
} castv2_state_t;

/* CASTV2 client instance */
typedef struct castv2_client_s {
    castv2_state_t state;
    int            sock_fd;         /* TCP socket to Chromecast */
    void*          ssl_ctx;         /* SSL_CTX* (void* to avoid OpenSSL in header) */
    void*          ssl;             /* SSL* */
    char           device_ip[64];   /* Chromecast IP address */
    char           device_name[128];/* Chromecast friendly name */
    char           transport_id[128]; /* DashCast transport session ID */
    char           stream_url[256]; /* URL to load on Chromecast */
    int            request_id;      /* Incrementing request counter */

    /* Heartbeat thread */
    pthread_t      heartbeat_thread;
    bool           heartbeat_running;
    pthread_mutex_t hb_mutex;
} castv2_client_t;

/**
 * @brief Initialize and start the MJPEG cast server
 * @param server Server instance
 * @param port TCP port to listen on (0 = default 8080)
 * @return true on success
 */
bool cast_server_init(cast_server_t* server, uint16_t port);

/**
 * @brief Push a new frame to the cast server
 */
void cast_server_push_frame(cast_server_t* server, const uint8_t* framebuffer,
                            unsigned int width, unsigned int height);

/**
 * @brief Push audio samples to the cast server ring buffer
 * @param server Server instance
 * @param stereo_samples Interleaved stereo PCM 16-bit samples (L,R,L,R,...)
 * @param num_samples Number of sample frames (each frame = 2 int16_t)
 */
void cast_server_push_audio(cast_server_t* server, const int16_t* stereo_samples,
                             size_t num_samples);

/**
 * @brief Stop and cleanup the cast server
 */
void cast_server_stop(cast_server_t* server);

/**
 * @brief Discover Chromecast devices on the network via mDNS/DNS-SD
 * @param timeout_ms Discovery timeout in milliseconds (0 = default 3000ms)
 * @return Number of devices found
 */
int cast_server_discover_devices(int timeout_ms);

/* Internal helpers (exposed for testing) */
void cast_server_upscale_nearest(const uint8_t* src, int src_w, int src_h,
                                 uint8_t* dst, int factor);
int  cast_server_build_mdns_query(uint8_t* buf, size_t buf_size);
int  cast_server_build_wav_header(uint8_t* buf, size_t buf_size);

/* ═══════════════════════════════════════════════════════════════════ */
/*  CASTV2 CLIENT API                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Discover a Chromecast device and return its IP
 * @param ip_out Buffer for IP address (min 64 bytes)
 * @param name_filter Device name filter (NULL = first found)
 * @param timeout_ms Discovery timeout (0 = default 3000ms)
 * @return true if device found
 */
bool castv2_discover_device(char* ip_out, const char* name_filter,
                            int timeout_ms);

/**
 * @brief Connect to Chromecast and cast a URL via DashCast
 * @param client Client instance (zeroed by caller)
 * @param device_ip Chromecast IP address
 * @param stream_url URL to load (e.g. http://192.168.1.10:8080/)
 * @return true on success
 */
bool castv2_connect_and_cast(castv2_client_t* client, const char* device_ip,
                             const char* stream_url);

/**
 * @brief Disconnect from Chromecast (send STOP, SSL shutdown, cleanup)
 * @param client Client instance
 */
void castv2_disconnect(castv2_client_t* client);

/**
 * @brief Get local IP address (for building stream URL)
 * @param ip_out Buffer for IP address (min 64 bytes)
 * @return true on success
 */
bool castv2_get_local_ip(char* ip_out);

/* Protobuf helpers (exposed for unit testing) */
int castv2_encode_varint(uint8_t* buf, int buf_size, uint64_t value);
int castv2_decode_varint(const uint8_t* buf, int buf_size, uint64_t* value);
int castv2_build_message(uint8_t* buf, int buf_size,
                         const char* source_id, const char* dest_id,
                         const char* ns, const char* payload);

#else /* !HAS_CAST */

/* Stub structure when cast is not compiled in */
typedef struct cast_server_s {
    uint16_t port;
    bool     active;
} cast_server_t;

typedef enum {
    CASTV2_STATE_IDLE = 0,
    CASTV2_STATE_ERROR
} castv2_state_t;

typedef struct castv2_client_s {
    castv2_state_t state;
    char           device_ip[64];
    char           device_name[128];
} castv2_client_t;

static inline bool cast_server_init(cast_server_t* s, uint16_t port) {
    (void)s; (void)port;
    return false;
}
static inline void cast_server_push_frame(cast_server_t* s,
                                          const uint8_t* fb,
                                          unsigned int w, unsigned int h) {
    (void)s; (void)fb; (void)w; (void)h;
}
static inline void cast_server_push_audio(cast_server_t* s,
                                           const int16_t* samples,
                                           size_t n) {
    (void)s; (void)samples; (void)n;
}
static inline void cast_server_stop(cast_server_t* s) {
    (void)s;
}
static inline int cast_server_discover_devices(int timeout_ms) {
    (void)timeout_ms;
    return 0;
}
static inline bool castv2_discover_device(char* ip_out, const char* name_filter,
                                          int timeout_ms) {
    (void)ip_out; (void)name_filter; (void)timeout_ms;
    return false;
}
static inline bool castv2_connect_and_cast(castv2_client_t* c, const char* ip,
                                           const char* url) {
    (void)c; (void)ip; (void)url;
    return false;
}
static inline void castv2_disconnect(castv2_client_t* c) {
    (void)c;
}
static inline bool castv2_get_local_ip(char* ip_out) {
    (void)ip_out;
    return false;
}

/* Protobuf stubs for testing */
static inline int castv2_encode_varint(uint8_t* buf, int buf_size, uint64_t value) {
    (void)buf; (void)buf_size; (void)value;
    return 0;
}
static inline int castv2_decode_varint(const uint8_t* buf, int buf_size, uint64_t* value) {
    (void)buf; (void)buf_size; (void)value;
    return 0;
}
static inline int castv2_build_message(uint8_t* buf, int buf_size,
                                       const char* src, const char* dst,
                                       const char* ns, const char* payload) {
    (void)buf; (void)buf_size; (void)src; (void)dst; (void)ns; (void)payload;
    return 0;
}

#endif /* HAS_CAST */

#endif /* CAST_SERVER_H */
