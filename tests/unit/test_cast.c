/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_cast.c
 * @brief Cast server unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-28
 * @version 1.2.0-alpha
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* stb header only (implementation is in cast_server.c) */
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb_image_write.h"

#include "network/cast_server.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected 0x%X, got 0x%X\n", __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════ */
/*  JPEG CALLBACK FOR TESTS                                            */
/* ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t* data;
    int      len;
    int      capacity;
} test_jpeg_buf_t;

static void test_jpeg_write_cb(void* context, void* data, int size) {
    test_jpeg_buf_t* buf = (test_jpeg_buf_t*)context;
    if (buf->len + size > buf->capacity) {
        int new_cap = (buf->len + size) * 2;
        if (new_cap < 4096) new_cap = 4096;
        uint8_t* new_data = (uint8_t*)realloc(buf->data, (size_t)new_cap);
        if (!new_data) return;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->len, data, (size_t)size);
    buf->len += size;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 1: UPSCALE SINGLE PIXEL                                      */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_upscale_single_pixel) {
    uint8_t src[3] = {0xFF, 0x00, 0x80};
    uint8_t dst[3 * 9] = {0}; /* 3x3 = 9 pixels */

    cast_server_upscale_nearest(src, 1, 1, dst, 3);

    /* All 9 pixels should be the same color */
    for (int i = 0; i < 9; i++) {
        ASSERT_EQ(dst[i * 3 + 0], 0xFF);
        ASSERT_EQ(dst[i * 3 + 1], 0x00);
        ASSERT_EQ(dst[i * 3 + 2], 0x80);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 2: UPSCALE 2x2 FRAME                                         */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_upscale_2x2_frame) {
    /* 2x2 source: R, G, B, W */
    uint8_t src[2 * 2 * 3] = {
        0xFF, 0x00, 0x00,  0x00, 0xFF, 0x00,  /* row 0: red, green */
        0x00, 0x00, 0xFF,  0xFF, 0xFF, 0xFF   /* row 1: blue, white */
    };
    uint8_t dst[4 * 4 * 3]; /* 4x4 with factor 2 */

    cast_server_upscale_nearest(src, 2, 2, dst, 2);

    /* Top-left 2x2 block should be red */
    ASSERT_EQ(dst[0], 0xFF); ASSERT_EQ(dst[1], 0x00); ASSERT_EQ(dst[2], 0x00);
    /* (1,0) in dst = still red (upscaled) */
    ASSERT_EQ(dst[3], 0xFF); ASSERT_EQ(dst[4], 0x00); ASSERT_EQ(dst[5], 0x00);
    /* (2,0) in dst = green */
    ASSERT_EQ(dst[6], 0x00); ASSERT_EQ(dst[7], 0xFF); ASSERT_EQ(dst[8], 0x00);

    /* Row 2 (y=2 in dst) should be blue, blue, white, white */
    int row2_off = 2 * 4 * 3; /* y=2, 4 pixels wide */
    ASSERT_EQ(dst[row2_off + 0], 0x00);
    ASSERT_EQ(dst[row2_off + 1], 0x00);
    ASSERT_EQ(dst[row2_off + 2], 0xFF); /* blue */
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 3: UPSCALE COLOR PRESERVATION                                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_upscale_color_preservation) {
    /* Single pixel with specific ORIC-like color */
    uint8_t src[3] = {0x00, 0xFF, 0x00}; /* green */
    uint8_t dst[3 * 4]; /* factor 2: 2x2 = 4 pixels */

    cast_server_upscale_nearest(src, 1, 1, dst, 2);

    /* All 4 pixels must be green */
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(dst[i * 3 + 0], 0x00);
        ASSERT_EQ(dst[i * 3 + 1], 0xFF);
        ASSERT_EQ(dst[i * 3 + 2], 0x00);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 4: JPEG ENCODE NON-ZERO LENGTH                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_jpeg_encode_nonzero) {
    int w = 8, h = 8;
    uint8_t img[8 * 8 * 3];
    for (int i = 0; i < w * h * 3; i++) {
        img[i] = (uint8_t)(i & 0xFF);
    }

    test_jpeg_buf_t jbuf = {NULL, 0, 0};
    stbi_write_jpg_to_func(test_jpeg_write_cb, &jbuf, w, h, 3, img, 80);

    ASSERT_TRUE(jbuf.len > 0);

    free(jbuf.data);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 5: JPEG SOI MARKER                                            */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_jpeg_soi_marker) {
    int w = 4, h = 4;
    uint8_t img[4 * 4 * 3];
    memset(img, 0x80, sizeof(img));

    test_jpeg_buf_t jbuf = {NULL, 0, 0};
    stbi_write_jpg_to_func(test_jpeg_write_cb, &jbuf, w, h, 3, img, 80);

    /* JPEG files start with SOI marker: 0xFF 0xD8 */
    ASSERT_TRUE(jbuf.len >= 2);
    ASSERT_EQ(jbuf.data[0], 0xFF);
    ASSERT_EQ(jbuf.data[1], 0xD8);

    free(jbuf.data);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 6: SERVER LIFECYCLE                                           */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_server_lifecycle) {
    cast_server_t server;

    /* Use a high port to avoid conflicts */
    bool ok = cast_server_init(&server, 18080);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(server.active);
    ASSERT_TRUE(server.thread_running);
    ASSERT_EQ(server.port, 18080);

    cast_server_stop(&server);
    ASSERT_FALSE(server.active);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 7: SERVER PORT ZERO DEFAULT                                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_server_port_default) {
    cast_server_t server;

    bool ok = cast_server_init(&server, 0);
    ASSERT_TRUE(ok);
    ASSERT_EQ(server.port, CAST_DEFAULT_PORT);

    cast_server_stop(&server);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 8: HTTP HTML PAGE CONTENT                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_http_html_page) {
    cast_server_t server;
    bool ok = cast_server_init(&server, 18081);
    ASSERT_TRUE(ok);

    /* Connect to server and request / */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(sock >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(18081);

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_EQ(ret, 0);

    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(sock, request, strlen(request), 0);

    /* Wait for response */
    usleep(100000); /* 100ms */
    char response[8192] = {0};
    recv(sock, response, sizeof(response) - 1, 0);

    /* Check for HTML content */
    ASSERT_TRUE(strstr(response, "text/html") != NULL);
    ASSERT_TRUE(strstr(response, "ORIC-1 Cast") != NULL);
    ASSERT_TRUE(strstr(response, "/snapshot") != NULL);
    ASSERT_TRUE(strstr(response, "AudioContext") != NULL);
    ASSERT_TRUE(strstr(response, "/audio") != NULL);

    close(sock);
    cast_server_stop(&server);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 9: HTTP MJPEG STREAM HEADER                                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_http_mjpeg_stream) {
    cast_server_t server;
    bool ok = cast_server_init(&server, 18082);
    ASSERT_TRUE(ok);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(sock >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(18082);

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_EQ(ret, 0);

    const char* request = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(sock, request, strlen(request), 0);

    usleep(100000);
    char response[4096] = {0};
    recv(sock, response, sizeof(response) - 1, 0);

    /* Check MJPEG boundary header */
    ASSERT_TRUE(strstr(response, "multipart/x-mixed-replace") != NULL);
    ASSERT_TRUE(strstr(response, "boundary=frame") != NULL);

    close(sock);
    cast_server_stop(&server);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 10: MDNS QUERY PACKET CONSTRUCTION                           */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_mdns_query_build) {
    uint8_t buf[128];
    int len = cast_server_build_mdns_query(buf, sizeof(buf));

    /* Should be a valid length */
    ASSERT_TRUE(len > 12); /* At least header + question */

    /* DNS Header: QDCOUNT = 1 */
    ASSERT_EQ(buf[4], 0);
    ASSERT_EQ(buf[5], 1);

    /* First label: 11 bytes = "_googlecast" */
    ASSERT_EQ(buf[12], 11);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 11: MDNS QUERY LENGTH                                        */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_mdns_query_length) {
    uint8_t buf[128];
    int len = cast_server_build_mdns_query(buf, sizeof(buf));

    /* Expected: 12 (header) + 1+11 + 1+4 + 1+5 + 1 (null) + 4 (type+class) = 40 */
    ASSERT_EQ(len, 40);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 12: MDNS QUERY BUFFER TOO SMALL                               */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_mdns_query_too_small) {
    uint8_t buf[10]; /* Too small */
    int len = cast_server_build_mdns_query(buf, sizeof(buf));

    ASSERT_EQ(len, -1);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 13: FRAME PUSH SETS READY FLAG                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_frame_push_ready) {
    cast_server_t server;
    bool ok = cast_server_init(&server, 18083);
    ASSERT_TRUE(ok);

    /* Initially no frame ready */
    ASSERT_FALSE(server.frame_ready);

    /* Push a small test frame */
    uint8_t frame[240 * 224 * 3];
    memset(frame, 0x42, sizeof(frame));
    cast_server_push_frame(&server, frame, 240, 224);

    /* Frame should be flagged as ready */
    pthread_mutex_lock(&server.mutex);
    ASSERT_TRUE(server.frame_ready);
    pthread_mutex_unlock(&server.mutex);

    cast_server_stop(&server);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  CASTV2 PROTOBUF TESTS                                              */
/* ═══════════════════════════════════════════════════════════════════ */

/* TEST 14: VARINT SINGLE BYTE */
TEST(test_varint_single) {
    uint8_t buf[16];
    int len = castv2_encode_varint(buf, sizeof(buf), 42);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(buf[0], 0x2A);
}

/* TEST 15: VARINT MULTI BYTE */
TEST(test_varint_multi) {
    uint8_t buf[16];
    int len = castv2_encode_varint(buf, sizeof(buf), 300);
    ASSERT_EQ(len, 2);
    ASSERT_EQ(buf[0], 0xAC);
    ASSERT_EQ(buf[1], 0x02);
}

/* TEST 16: VARINT ROUNDTRIP */
TEST(test_varint_roundtrip) {
    uint64_t test_values[] = {0, 1, 42, 127, 128, 300, 16384};
    int count = (int)(sizeof(test_values) / sizeof(test_values[0]));

    for (int i = 0; i < count; i++) {
        uint8_t buf[16];
        int enc_len = castv2_encode_varint(buf, sizeof(buf), test_values[i]);
        ASSERT_TRUE(enc_len > 0);

        uint64_t decoded;
        int dec_len = castv2_decode_varint(buf, enc_len, &decoded);
        ASSERT_EQ(dec_len, enc_len);
        ASSERT_TRUE(decoded == test_values[i]);
    }
}

/* TEST 17: BUILD MESSAGE POSITIVE LENGTH */
TEST(test_build_message) {
    uint8_t buf[4096];
    int len = castv2_build_message(buf, sizeof(buf),
                                   "sender-0", "receiver-0",
                                   "urn:x-cast:com.google.cast.tp.connection",
                                   "{\"type\":\"CONNECT\"}");
    ASSERT_TRUE(len > 0);
}

/* TEST 18: MESSAGE FRAMING (4 bytes big-endian length) */
TEST(test_message_framing) {
    uint8_t buf[4096];
    int len = castv2_build_message(buf, sizeof(buf),
                                   "sender-0", "receiver-0",
                                   "urn:x-cast:com.google.cast.tp.heartbeat",
                                   "{\"type\":\"PING\"}");
    ASSERT_TRUE(len > 4);

    /* First 4 bytes = big-endian length of protobuf message */
    uint32_t msg_len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                       ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    ASSERT_EQ((int)msg_len, len - 4);
}

/* TEST 19: MESSAGE CONTAINS SOURCE ID */
TEST(test_message_contains_source) {
    uint8_t buf[4096];
    int len = castv2_build_message(buf, sizeof(buf),
                                   "sender-0", "receiver-0",
                                   "urn:x-cast:com.google.cast.tp.connection",
                                   "{\"type\":\"CONNECT\"}");
    ASSERT_TRUE(len > 0);

    /* Search for "sender-0" in the protobuf payload */
    bool found = false;
    for (int i = 4; i < len - 8; i++) {
        if (memcmp(buf + i, "sender-0", 8) == 0) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
}

/* TEST 20: MESSAGE BUFFER TOO SMALL */
TEST(test_message_buffer_too_small) {
    uint8_t buf[16]; /* Too small for a complete message */
    int len = castv2_build_message(buf, sizeof(buf),
                                   "sender-0", "receiver-0",
                                   "urn:x-cast:com.google.cast.tp.connection",
                                   "{\"type\":\"CONNECT\"}");
    ASSERT_EQ(len, -1);
}

/* TEST 21: MESSAGE PROTOCOL VERSION FIELD */
TEST(test_message_protocol_version) {
    uint8_t buf[4096];
    int len = castv2_build_message(buf, sizeof(buf),
                                   "sender-0", "receiver-0",
                                   "urn:x-cast:com.google.cast.tp.connection",
                                   "{\"type\":\"CONNECT\"}");
    ASSERT_TRUE(len > 6);

    /* After 4-byte framing header, first protobuf field should be:
     * Field 1 (protocol_version), wire type 0 (varint): tag = (1 << 3 | 0) = 0x08
     * Value = 0: 0x00 */
    ASSERT_EQ(buf[4], 0x08);
    ASSERT_EQ(buf[5], 0x00);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  AUDIO STREAMING TESTS                                              */
/* ═══════════════════════════════════════════════════════════════════ */

/* TEST 22: WAV HEADER VALIDITY */
TEST(test_wav_header) {
    uint8_t hdr[44];
    int len = cast_server_build_wav_header(hdr, sizeof(hdr));
    ASSERT_EQ(len, 44);

    /* RIFF magic */
    ASSERT_EQ(hdr[0], 'R'); ASSERT_EQ(hdr[1], 'I');
    ASSERT_EQ(hdr[2], 'F'); ASSERT_EQ(hdr[3], 'F');

    /* WAVE magic */
    ASSERT_EQ(hdr[8], 'W'); ASSERT_EQ(hdr[9], 'A');
    ASSERT_EQ(hdr[10], 'V'); ASSERT_EQ(hdr[11], 'E');

    /* fmt sub-chunk */
    ASSERT_EQ(hdr[12], 'f'); ASSERT_EQ(hdr[13], 'm');
    ASSERT_EQ(hdr[14], 't'); ASSERT_EQ(hdr[15], ' ');

    /* Audio format = 1 (PCM) */
    uint16_t audio_fmt;
    memcpy(&audio_fmt, hdr + 20, 2);
    ASSERT_EQ(audio_fmt, 1);

    /* Channels = 1 (mono) */
    uint16_t channels;
    memcpy(&channels, hdr + 22, 2);
    ASSERT_EQ(channels, 1);

    /* Sample rate = 44100 */
    uint32_t sample_rate;
    memcpy(&sample_rate, hdr + 24, 4);
    ASSERT_EQ(sample_rate, 44100);

    /* Bits per sample = 16 */
    uint16_t bps;
    memcpy(&bps, hdr + 34, 2);
    ASSERT_EQ(bps, 16);

    /* data sub-chunk */
    ASSERT_EQ(hdr[36], 'd'); ASSERT_EQ(hdr[37], 'a');
    ASSERT_EQ(hdr[38], 't'); ASSERT_EQ(hdr[39], 'a');
}

/* TEST 23: AUDIO RING BUFFER PUSH */
TEST(test_audio_ring_push) {
    cast_server_t server;
    bool ok = cast_server_init(&server, 18090);
    ASSERT_TRUE(ok);

    /* Initial write position is 0 */
    ASSERT_EQ(server.audio_write_pos, 0);

    /* Push 4 stereo samples (8 int16_t values) */
    int16_t stereo[8] = {100, 200, 300, 400, 500, 600, 700, 800};
    cast_server_push_audio(&server, stereo, 4);

    /* Write position should advance by 4 */
    ASSERT_EQ(server.audio_write_pos, 4);

    /* Check mono downmix: (L+R)/2 */
    ASSERT_EQ(server.audio_ring[0], 150);  /* (100+200)/2 */
    ASSERT_EQ(server.audio_ring[1], 350);  /* (300+400)/2 */
    ASSERT_EQ(server.audio_ring[2], 550);  /* (500+600)/2 */
    ASSERT_EQ(server.audio_ring[3], 750);  /* (700+800)/2 */

    cast_server_stop(&server);
}

/* TEST 24: AUDIO RING BUFFER WRAP-AROUND */
TEST(test_audio_ring_wrap) {
    cast_server_t server;
    bool ok = cast_server_init(&server, 18091);
    ASSERT_TRUE(ok);

    /* Set write position near the end of the ring buffer */
    server.audio_write_pos = CAST_AUDIO_RING_SAMPLES - 2;

    /* Push 4 stereo samples — should wrap around */
    int16_t stereo[8] = {1000, 1000, 2000, 2000, 3000, 3000, 4000, 4000};
    cast_server_push_audio(&server, stereo, 4);

    /* Write position should have wrapped: (N-2+4) % N = 2 */
    ASSERT_EQ(server.audio_write_pos, 2);

    /* Check samples at end of buffer */
    ASSERT_EQ(server.audio_ring[CAST_AUDIO_RING_SAMPLES - 2], 1000);
    ASSERT_EQ(server.audio_ring[CAST_AUDIO_RING_SAMPLES - 1], 2000);

    /* Check wrapped samples at start of buffer */
    ASSERT_EQ(server.audio_ring[0], 3000);
    ASSERT_EQ(server.audio_ring[1], 4000);

    cast_server_stop(&server);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Cast Server + CASTV2 + Audio Streaming Unit Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    /* Cast Server tests (13) */
    RUN(test_upscale_single_pixel);
    RUN(test_upscale_2x2_frame);
    RUN(test_upscale_color_preservation);
    RUN(test_jpeg_encode_nonzero);
    RUN(test_jpeg_soi_marker);
    RUN(test_server_lifecycle);
    RUN(test_server_port_default);
    RUN(test_http_html_page);
    RUN(test_http_mjpeg_stream);
    RUN(test_mdns_query_build);
    RUN(test_mdns_query_length);
    RUN(test_mdns_query_too_small);
    RUN(test_frame_push_ready);

    /* CASTV2 Protobuf tests (8) */
    printf("\n  --- CASTV2 Protobuf ---\n");
    RUN(test_varint_single);
    RUN(test_varint_multi);
    RUN(test_varint_roundtrip);
    RUN(test_build_message);
    RUN(test_message_framing);
    RUN(test_message_contains_source);
    RUN(test_message_buffer_too_small);
    RUN(test_message_protocol_version);

    /* Audio Streaming tests (3) */
    printf("\n  --- Audio Streaming ---\n");
    RUN(test_wav_header);
    RUN(test_audio_ring_push);
    RUN(test_audio_ring_wrap);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
