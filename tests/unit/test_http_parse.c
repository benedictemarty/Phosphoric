/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_http_parse.c
 * @brief Parsing and routing of HTTP API requests, without network or emulator
 * @author bmarty <bmarty@mailo.com>
 *
 * Includes src/network/http_api.c to reach its static functions (same idea
 * as tests/fuzz/fuzz_http.c): URL decoding, parameters, path sandbox,
 * Content-Length, produced --control command. Built in every configuration
 * (HAS_HTTPAPI forced here), unlike test-httpapi (end to end, HTTPAPI=1 only).
 */
#ifndef HAS_HTTPAPI
#define HAS_HTTPAPI 1
#endif
#include "../../src/network/http_api.c"   /* first: _GNU_SOURCE */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-52s", #name); name(); tests_passed++; printf("PASS\n"); } while (0)
#define ASSERT_TRUE(x) do { if (!(x)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #x); tests_failed++; return; } } while (0)
#define ASSERT_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: attendu \"%s\", obtenu \"%s\"\n", __FILE__, __LINE__, (b), (a)); \
        tests_failed++; return; } } while (0)

/* The queue to the emulator is not used here. */
control_result_t control_queue_submit(control_queue_t* q, const char* line,
                                      char** reply_out, size_t* reply_len) {
    (void)q; (void)line;
    if (reply_out) *reply_out = NULL;
    if (reply_len) *reply_len = 0;
    return CONTROL_CONTINUE;
}

static http_api_server_t g_srv;
static int g_sv[2] = { -1, -1 };

/* Sends @p raw to handle_request(). Returns true if a command is produced
 * (in @p cmd); otherwise the HTTP response sent is copied into @p resp. */
static bool request(const char* raw, char* cmd, size_t cmdsz, char* resp, size_t respsz) {
    char req[HTTP_REQ_MAX];
    size_t n = strlen(raw);
    memcpy(req, raw, n + 1);
    bool r = handle_request(&g_srv, g_sv[0], req, n, cmd, cmdsz);
    ssize_t got = recv(g_sv[1], resp, respsz - 1, 0);
    resp[got > 0 ? got : 0] = '\0';
    return r;
}

TEST(test_url_decode) {
    char a[] = "A%20B+C%2fd";
    url_decode(a);
    ASSERT_STR(a, "A B C/d");
    char b[] = "100%zz%4";              /* % without two hex digits: kept */
    url_decode(b);
    ASSERT_STR(b, "100%zz%4");
    char c[] = "%\x01" "1";             /* old code: shifted a negative value */
    url_decode(c);
    ASSERT_STR(c, "%\x01" "1");
}

TEST(test_get_param) {
    char out[16];
    ASSERT_TRUE(get_param("addr=0x400&len=16", "len", out, sizeof out));
    ASSERT_STR(out, "16");
    ASSERT_TRUE(!get_param("addr=1", "len", out, sizeof out));
    ASSERT_TRUE(get_param("text=0123456789ABCDEFGHIJ", "text", out, sizeof out));
    ASSERT_STR(out, "0123456789ABCDE");  /* truncated to the buffer size */
}

TEST(test_sandbox_path) {
    char full[HTTP_ROOT_MAX];
    ASSERT_TRUE(sandbox_path(&g_srv, "jeux/a.dsk", full, sizeof full));
    ASSERT_STR(full, "/srv/oric/jeux/a.dsk");
    ASSERT_TRUE(!sandbox_path(&g_srv, "/etc/passwd", full, sizeof full));
    ASSERT_TRUE(!sandbox_path(&g_srv, "jeux/../../etc", full, sizeof full));
    ASSERT_TRUE(!sandbox_path(&g_srv, "", full, sizeof full));
}

TEST(test_route_get_mem) {
    char cmd[HTTP_CMD_MAX], resp[4096];
    ASSERT_TRUE(request("GET /mem?addr=0400&len=16&bank=ram HTTP/1.1\r\n\r\n",
                        cmd, sizeof cmd, resp, sizeof resp));
    ASSERT_STR(cmd, "read 0400 16 ram");
}

TEST(test_route_post_disk_sandboxed) {
    char cmd[HTTP_CMD_MAX], resp[4096];
    ASSERT_TRUE(request("POST /disk/A HTTP/1.1\r\nContent-Length: 19\r\n\r\npath=jeux%2Fa.dsk&x",
                        cmd, sizeof cmd, resp, sizeof resp));
    ASSERT_STR(cmd, "load-disk A /srv/oric/jeux/a.dsk");
    /* %2F%2E%2E decoded to "/..": refused like a plain "..". */
    ASSERT_TRUE(!request("POST /disk/A HTTP/1.1\r\nContent-Length: 22\r\n\r\npath=a%2F%2E%2E%2Fetc",
                         cmd, sizeof cmd, resp, sizeof resp));
    ASSERT_TRUE(strstr(resp, "403") != NULL);
}

TEST(test_content_length_clamped) {
    char cmd[HTTP_CMD_MAX], resp[4096];
    /* Huge announced length: clamped to what was received (old code:
     * body_start + content_len overflowed). */
    ASSERT_TRUE(request("POST /keys HTTP/1.1\r\nContent-Length: 18446744073709551615\r\n\r\ntext=AB",
                        cmd, sizeof cmd, resp, sizeof resp));
    ASSERT_STR(cmd, "keys AB");
    /* Length shorter than the body: the body is cut. */
    ASSERT_TRUE(request("POST /keys HTTP/1.1\r\nContent-Length: 6\r\n\r\ntext=ABCD",
                        cmd, sizeof cmd, resp, sizeof resp));
    ASSERT_STR(cmd, "keys A");
}

TEST(test_malformed_and_unknown) {
    char cmd[HTTP_CMD_MAX], resp[4096];
    ASSERT_TRUE(!request("GARBAGE\r\n\r\n", cmd, sizeof cmd, resp, sizeof resp));
    ASSERT_TRUE(strstr(resp, "400") != NULL);
    ASSERT_TRUE(!request("POST /exec/rm HTTP/1.1\r\n\r\n", cmd, sizeof cmd, resp, sizeof resp));
    ASSERT_TRUE(strstr(resp, "404") != NULL);
    ASSERT_TRUE(!request("GET /mem HTTP/1.1\r\n", cmd, sizeof cmd, resp, sizeof resp));  /* incomplete header */
}

int main(void) {
    printf("=== API HTTP : analyse et routage (sans réseau) ===\n");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_sv) != 0) { perror("socketpair"); return 1; }
    set_nonblocking(g_sv[1]);
    snprintf(g_srv.root, sizeof g_srv.root, "/srv/oric");
    log_init(LOG_LEVEL_ERROR);

    RUN(test_url_decode);
    RUN(test_get_param);
    RUN(test_sandbox_path);
    RUN(test_route_get_mem);
    RUN(test_route_post_disk_sandboxed);
    RUN(test_content_length_clamped);
    RUN(test_malformed_and_unknown);

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
