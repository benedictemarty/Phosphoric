/**
 * @file appsignal.h
 * @brief Gestion des signaux d'arrêt (SIGINT/SIGTERM) du processus.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extrait de main.c (Epic 7 / US1, Sprint 125). Encapsule le drapeau global
 * « le processus doit-il continuer ? » et l'installation des handlers.
 */
#ifndef APPSIGNAL_H
#define APPSIGNAL_H

#include <stdbool.h>

/** Installe les handlers SIGINT/SIGTERM (mettent le drapeau à false). */
void app_install_signal_handlers(void);

/** Vrai tant qu'aucun signal d'arrêt n'a été reçu. */
bool app_should_run(void);

#endif /* APPSIGNAL_H */
