/**
 * @file http_api.h
 * @brief Optional HTTP control API (sprint 94, API REST Epic 3).
 *
 * A tiny HTTP/1.1 server, running in its own thread, that turns REST calls
 * into `--control` commands and executes them on the emulator thread through
 * the thread-safe control_queue (drained once per frame by the main loop).
 * This is the transport built on top of the sprint 92 dispatch + sprint 93
 * queue; no command logic is duplicated here.
 *
 * Built only with HTTPAPI=1 (defines HAS_HTTPAPI). Without it the public
 * functions collapse to no-op stubs so main.c needs no #ifdef around calls.
 *
 * Security: file-loading endpoints (/tape, /disk) resolve paths **inside**
 * a sandbox root (default: CWD); absolute paths and ".." are rejected. The
 * listener binds 127.0.0.1 by default; expose it deliberately with a bind
 * address of "0.0.0.0".
 */

#ifndef HTTP_API_H
#define HTTP_API_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct emulator_s emulator_t;
typedef struct control_queue_s control_queue_t;

#define HTTP_API_DEFAULT_PORT 8888
#define HTTP_API_DEFAULT_BIND "127.0.0.1"

#ifdef HAS_HTTPAPI

typedef struct http_api_server_s http_api_server_t;

/**
 * @brief Create and start the HTTP API server thread.
 *
 * @param emu    emulator (commands run on its thread via the queue)
 * @param queue  shared command queue; the server is the producer
 * @param port   TCP port (0 → HTTP_API_DEFAULT_PORT)
 * @param bind   bind address (NULL → 127.0.0.1)
 * @param root   sandbox root for file ops (NULL → "." current dir)
 * @return server handle, or NULL on failure (a message is logged).
 */
http_api_server_t* http_api_start(emulator_t* emu, control_queue_t* queue,
                                  uint16_t port, const char* bind,
                                  const char* root);

/** Stop the server thread and free the handle. Safe with NULL. */
void http_api_stop(http_api_server_t* server);

#else /* !HAS_HTTPAPI — no-op stubs */

typedef struct http_api_server_s http_api_server_t;

static inline http_api_server_t* http_api_start(emulator_t* emu,
                                                control_queue_t* queue,
                                                uint16_t port, const char* bind,
                                                const char* root) {
    (void)emu; (void)queue; (void)port; (void)bind; (void)root;
    return NULL;
}
static inline void http_api_stop(http_api_server_t* server) { (void)server; }

#endif /* HAS_HTTPAPI */

#endif /* HTTP_API_H */
