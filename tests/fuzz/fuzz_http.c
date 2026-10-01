/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_http.c
 * @brief Fuzzing: HTTP request → API parsing and routing (http_api.c)
 * @author bmarty <bmarty@mailo.com>
 *
 * Includes http_api.c to reach its static functions: handle_request()
 * (request line, Content-Length, parameters, path sandbox, building the
 * --control command) then respond_reply() (JSON escaping) on the produced
 * command. Nothing is executed: the command is not handed to the emulator.
 * Responses go to a socket pair drained after every input.
 */
#ifndef HAS_HTTPAPI
#define HAS_HTTPAPI 1   /* the server is compiled whatever the build */
#endif
#include "../../src/network/http_api.c"   /* first: _GNU_SOURCE */
#include "fuzz_common.h"

FUZZ_NO_SEED()

/* The queue to the emulator is never reached (handle_client is not
 * called); this stub avoids linking the whole control.c dispatcher. */
control_result_t control_queue_submit(control_queue_t* q, const char* line,
                                      char** reply_out, size_t* reply_len) {
    (void)q; (void)line;
    if (reply_out) *reply_out = NULL;
    if (reply_len) *reply_len = 0;
    return CONTROL_CONTINUE;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static int sv[2] = { -1, -1 };
    static http_api_server_t srv;
    FUZZ_QUIET_LOGS();
    if (sv[0] < 0) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) abort();
        set_nonblocking(sv[1]);
        snprintf(srv.root, sizeof srv.root, "/tmp/phosphoric_fuzz_root");
    }
    char req[HTTP_REQ_MAX];
    size_t n = size < sizeof req - 1 ? size : sizeof req - 1;
    memcpy(req, data, n);
    req[n] = '\0';

    char cmd[HTTP_CMD_MAX];
    if (handle_request(&srv, sv[0], req, n, cmd, sizeof cmd))
        respond_reply(sv[0], cmd);

    char drain[4096];
    while (recv(sv[1], drain, sizeof drain, 0) > 0) { }
    return 0;
}
