/**
 * @file appsignal.c
 * @brief Gestion des signaux d'arrêt — implémentation.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extrait de main.c (Epic 7 / US1, Sprint 125), à l'identique.
 */
#include "utils/appsignal.h"

#include <signal.h>

static volatile bool g_running = true;

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

void app_install_signal_handlers(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
}

bool app_should_run(void) {
    return g_running;
}
