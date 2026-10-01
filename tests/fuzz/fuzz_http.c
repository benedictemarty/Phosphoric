/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_http.c
 * @brief Fuzzing : requête HTTP → analyse et routage de l'API (http_api.c)
 * @author bmarty <bmarty@mailo.com>
 *
 * Inclut http_api.c pour atteindre ses fonctions static : handle_request()
 * (ligne de requête, Content-Length, paramètres, bac à sable des chemins,
 * fabrication de la commande --control) puis respond_reply() (échappement JSON)
 * sur la commande produite. Rien n'est exécuté : la commande n'est pas
 * transmise à l'émulateur. Les réponses partent dans une paire de sockets
 * vidée après chaque entrée.
 */
#ifndef HAS_HTTPAPI
#define HAS_HTTPAPI 1   /* le serveur est compilé quelle que soit la build */
#endif
#include "../../src/network/http_api.c"   /* en premier : _GNU_SOURCE */
#include "fuzz_common.h"

FUZZ_NO_SEED()

/* La file vers l'émulateur n'est jamais atteinte (handle_client n'est pas
 * appelé) ; ce bouchon évite de lier tout le répartiteur control.c. */
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
